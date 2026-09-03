/**
 * @file study_mode_machine.c
 * @brief 学习模式状态机实现 (Task F-16)
 *
 * 语义动作由五向导航键映射（上下翻词/中发音/SET 揭晓/RST 回首）；
 * 切换模式时记录到 NVS 以便下次开机恢复。
 */
#include "study_mode_machine.h"
#include "debug_log.h"
#include "gpio_config.h"
#include "epd_driver.h"
#include "audio_player.h"
#include "storage_manager.h"   /* P0C：speak 文件存在性预检 */
#include "haptic.h"            /* P0C：缺音频短震反馈（替代测试音兜底） */
#include "word_parser.h"
#include "mic_recorder.h"      /* P1：跟读录音+上传 */
#include "wifi_manager.h"      /* P1：跟读前置 Wi-Fi 检查 */
#include "chat_mode.h"         /* P2B：对话模式任务生命周期 */
#include "sync_client.h"       /* P2B：enter_chat 前置 Key 检查 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>           /* pron_flow_start strncpy（P1） */
#include <sys/stat.h>           /* P2B：enter_chat SD 在位预检 */
#include <errno.h>
#include "learning_state.h"
#include "settings_ui.h"  /* v1.2 T2.5：发音门控（set_audio） */
#include "page_router.h" /* T1.4：渲染恢复经 render_top（pron 恢复路径） */
#include "reader_engine.h"   /* READER 模式：页序列/字号切换/进度恢复 */

#include "nvs_flash.h"
#include "nvs.h"
#include "settings_keys.h"   /* P2b：NVS 键权威表 */

static const char *TAG = "MODE";

static study_mode_t s_current = MODE_FLASH;

/* ---- 跟读评测编排（P1：听-跟一体流，任务化不阻塞按键） ---- */
static volatile bool s_pron_active  = false; /* 任务在跑（录音/上传） */
static volatile bool s_pron_showing = false; /* 结果/失败屏亮着等键退出 */
static volatile bool s_pron_cancel  = false; /* 录音循环逐块检查的取消位 */
static char s_pron_cloud_id[WORD_CLOUD_ID_MAX];

/* main.cpp 导出（C++ → C，ui_render_word 引用同款先例） */
extern void ui_render_pron(pron_state_t st, int total, const char *engine);

/* 当前词索引（各模式独立游标演示，实际可扩展为独立游标） */
static int s_cursor = 0;

/* 释义显示状态：true=显示（默认，与历史行为一致），false=遮蔽（自测）；
 * SET 翻义切换；翻页/切模式自动回显示（新词全显，按 SET 开始遮蔽自测） */
static bool s_reveal = true;

static const char *s_names[MODE_COUNT] =
    { "Flash", "Dictation", "Review", "Reader", "WrongBook", "收藏",
      "AI Chat", "测验", "目录", "语音" };

/* ---- 序列抽象：默认全词库，错词本换连错过滤视图，阅读换页序列 ---- */

static int seq_total(void)
{
    if (s_current == MODE_WRONGBOOK)  return learning_state_wrong_count();
    if (s_current == MODE_COLLECTION) return learning_state_collected_count();
    if (s_current == MODE_REVIEW)     return learning_state_due_count();
    if (s_current == MODE_READER)     return reader_page_count();
    if (s_current == MODE_CHAT)       return 0;  /* 对话无词序列（状态栏 0/0） */
    if (s_current == MODE_QUIZ)       return 0;  /* 测验题号由 main.cpp 自绘状态栏 */
    if (s_current == MODE_BROWSE)     return 0;  /* 目录页码由 browse_mode 自绘 */
    if (s_current == MODE_VOICE)      return 0;  /* 候选列表由 voice_search 自绘 */
    return word_parser_get_count();
}

/* 进入阅读模式时恢复上次阅读页（书签名匹配才有效，否则回第 0 页） */
static void reader_cursor_restore(void)
{
    s_cursor = 0;
    int p = reader_progress_page();
    if (p >= 0) s_cursor = p;
}

/* 游标 -> 词库索引（错词本/收藏/复习模式下为过滤序列内第 cursor 个词；
 * 复习=FSRS 到期视图，2026-08-24 PRD「复习=SRS 到期词」落地） */
static int seq_word_index(int cursor)
{
    if (s_current == MODE_WRONGBOOK)  return learning_state_wrong_at(cursor);
    if (s_current == MODE_COLLECTION) return learning_state_collected_at(cursor);
    if (s_current == MODE_REVIEW)     return learning_state_due_at(cursor);
    return cursor;
}

void study_mode_init(void)
{
    /* 从 NVS 恢复上次模式（错词本/收藏/对话是临时视图，不接受恢复） */
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t m = 0;
        if (nvs_get_u8(h, NVS_KEY_LAST_MODE, &m) == ESP_OK &&
            m < MODE_COUNT && m != MODE_WRONGBOOK && m != MODE_COLLECTION &&
            m != MODE_CHAT && m != MODE_QUIZ && m != MODE_BROWSE &&
            m != MODE_VOICE) {
            s_current = (study_mode_t)m;
        }
        nvs_close(h);
    }
    s_cursor = 0;
    if (s_current == MODE_READER) reader_cursor_restore();
    LOG_I("study mode initialized: %s", s_names[s_current]);
}

study_mode_t study_mode_current(void)
{
    return s_current;
}

/* 模式切换公共副作用（switch_next / study_mode_set 共用）：游标归零 /
 * READER 进度恢复 / 遮蔽复位 / NVS 持久化。临时视图（错词本/收藏）
 * 由 enter_* 显式进入，不经 apply_mode。 */
static void apply_mode(study_mode_t mode)
{
    s_current = mode;
    s_cursor = 0;
    if (s_current == MODE_READER) reader_cursor_restore();
    s_reveal = true;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_LAST_MODE, (uint8_t)s_current);
        nvs_commit(h);
        nvs_close(h);
    }

    LOG_I("mode switched -> %s", s_names[s_current]);
}

study_mode_t study_mode_switch_next(void)
{
    /* 跳过临时视图（do-while 保证至少前进一步，不会死循环） */
    do {
        s_current = (study_mode_t)((s_current + 1) % MODE_COUNT);
    } while (s_current == MODE_WRONGBOOK || s_current == MODE_COLLECTION ||
             s_current == MODE_CHAT || s_current == MODE_QUIZ ||
             s_current == MODE_BROWSE || s_current == MODE_VOICE);
    apply_mode(s_current);
    return s_current;
}

void study_mode_set(study_mode_t mode)
{
    if (mode < 0 || mode >= MODE_COUNT) return;
    if (mode == MODE_WRONGBOOK || mode == MODE_COLLECTION ||
        mode == MODE_CHAT || mode == MODE_QUIZ ||
        mode == MODE_BROWSE || mode == MODE_VOICE) return;
    apply_mode(mode);   /* 同模式重入也归零游标，与 switch_next 语义一致 */
}

const char *study_mode_name(study_mode_t mode)
{
    return (mode >= 0 && mode < MODE_COUNT) ? s_names[mode] : "Unknown";
}

/* ---- 模式相关渲染桩：实际由 UI 渲染模块填充 ----
 * T2.2 base 收敛：翻页/重置/seek/字号四处直渲改 page_router_render_top
 * （base_render 单入口分流，current_word_index 同口径闭环） */
#include "page_router.h"

/* ---- 跟读评测编排（P1：听-跟一体流，任务化不阻塞按键） ----
 * 反馈优先 haptic（PRD 5.4）：录音起一短震、≥60 双短震（HAPTIC_PASS
 * 预留位正配）、<60 一长震（HAPTIC_FAIL）。网络失败丢弃本次不缓存
 * （3s WAV 重录成本低于缓存复杂度，偏离 SPEECH 文档缓存项，计划裁定） */
static void pron_task(void *arg)
{
    (void)arg;

    /* 等发音播完（上限 8s：超长音频兜底；否则录音会采到喇叭声） */
    int waited = 0;
    while (audio_is_playing() && waited < 8000) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    if (s_pron_cancel) goto restore;

    ui_render_pron(PRON_STATE_RECORDING, 0, NULL);
    haptic_event(HAPTIC_KEYPRESS);          /* 录音起一短震 */

    uint8_t *wav = NULL;
    size_t wav_len = 0;
    int rc = mic_recorder_record(&wav, &wav_len, &s_pron_cancel, NULL, 3000);
    if (rc == 0) {
        ui_render_pron(PRON_STATE_SCORING, 0, NULL);
        pron_result_t res;
        rc = mic_recorder_upload(s_pron_cloud_id, wav, wav_len, &res);
        free(wav);
        if (rc == 0) {
            LOG_I("pron %s score=%d engine=%s dur=%dms", s_pron_cloud_id,
                  res.total, res.engine, res.duration_ms);
            haptic_event(res.total >= 60 ? HAPTIC_PASS : HAPTIC_FAIL);
            ui_render_pron(PRON_STATE_RESULT, res.total, res.engine);
            s_pron_showing = true;
            s_pron_active = false;
            vTaskDelete(NULL);
            return;
        }
    } else if (rc == -2) {
        goto restore;                       /* 用户取消：不提示直接回词卡 */
    }

    /* 失败（录音/I2S/网络/解析；无声单独提示，rc 经 total 透传） */
    haptic_event(HAPTIC_ERROR);
    ui_render_pron(PRON_STATE_FAIL, rc, NULL);
    s_pron_showing = true;
    s_pron_active = false;
    vTaskDelete(NULL);
    return;

restore:
    s_pron_active = false;   /* 先清位再渲染（ui_render_word 检查 pron 态） */
    s_pron_cancel = false;
    page_router_render_top();
    vTaskDelete(NULL);
}

static void pron_flow_start(const char *cloud_id)
{
    strncpy(s_pron_cloud_id, cloud_id, sizeof(s_pron_cloud_id) - 1);
    s_pron_cloud_id[sizeof(s_pron_cloud_id) - 1] = '\0';
    s_pron_cancel = false;
    s_pron_active = true;   /* 先置位再建任务（瞬间跑完竞态防护） */
    if (xTaskCreate(pron_task, "pron", 6 * 1024, NULL, 4, NULL) != pdPASS) {
        s_pron_active = false;
        LOG_E("pron task create failed");
    }
}

bool study_mode_pron_active(void)    { return s_pron_active; }
bool study_mode_pron_ui_visible(void) { return s_pron_showing; }

void study_mode_pron_any_key(void)
{
    if (s_pron_active) {
        s_pron_cancel = true;   /* 任务收尾自恢复词卡 */
    } else if (s_pron_showing) {
        s_pron_showing = false;
        page_router_render_top();
    }
}

/* ---- 语义动作处理 ---- */
void study_mode_handle_action(int action)
{
    int total = seq_total();

    switch (action) {
    case 0: /* prev：边界钳制不回绕（2026-09-03：词 0 上翻回绕到词库
     * 尾部——2407 条混排词库下误入高考古诗文区段，学习序列断裂；
     * 首条上翻=长震拒绝，游标原地，speak 缺音频同款单震反馈） */
        if (total <= 0) return;         /* 空序列（READER 无书）不动作 */
        if (s_cursor <= 0) {
            haptic_event(HAPTIC_ERROR);
            return;
        }
        s_cursor--;
        s_reveal = true;
        break;
    case 1: /* next：末条下翻同钳制（对称；READER 末页=书读尽拒翻） */
        if (total <= 0) return;
        if (s_cursor >= total - 1) {
            haptic_event(HAPTIC_ERROR);
            return;
        }
        s_cursor++;
        s_reveal = true;
        break;
    case 2: /* confirm：闪卡模式翻转释义，听写模式提交拼写 */
        if (s_current == MODE_READER) return;  /* 书页无遮蔽语义 */
        s_reveal = !s_reveal;
        LOG_D("confirm action in %s mode (reveal=%d)",
              s_names[s_current], s_reveal);
        break;
    case 3: { /* speak：播放当前词音频（P0C 命名解析）。w->audio 人工
               * 命名词库优先，否则按云端约定取 {cloud_id}.mp3；文件
               * 缺失（含无 SD）短震反馈，不再回退测试音（TEMP 已移除）。
               * 异步入队即返，存在性在此先行检查。
               * v1.2 T2.5：发音关闭（set_audio=0）时整段跳过
               * （不播不进跟读，提示音门控在 ui_sfx_play 入口） */
        if (s_current == MODE_READER) return;  /* 书页无词音频 */
        if (!settings_audio_enabled()) break;
        const WordEntry *w = word_parser_get(seq_word_index(s_cursor));
        char path[128] = { 0 };
        if (w && w->audio[0])
            snprintf(path, sizeof(path), "%s/%s", AUDIO_DIR, w->audio);
        else if (w && w->cloud_id[0])
            snprintf(path, sizeof(path), "%s/%s.mp3", AUDIO_DIR, w->cloud_id);

        if (!path[0] || !storage_file_exists(path)) {
            LOG_W("speak: no audio '%s' (cloud_id=%s)",
                  w ? w->text : "?", (w && w->cloud_id[0]) ? w->cloud_id : "-");
            haptic_event(HAPTIC_ERROR);
            break;
        }
        if (audio_play_file(path) != 0)
            LOG_W("speak: enqueue rejected (recording suspended?)");

        /* P1 听-跟一体流：云端词播完自动进跟读（本地词零打扰，仅播）。
         * 前置 Wi-Fi/未在跑；录音等播完由 pron_task 自理 */
        if (w && w->cloud_id[0] && wifi_is_connected() &&
            !study_mode_pron_active() && !study_mode_pron_ui_visible())
            pron_flow_start(w->cloud_id);
        break;
    }
    default:
        break;
    }

    /* 仅画面变化的动作触发重绘：翻页(prev/next)与翻义(confirm) 局刷内容区；
     * speak(3) 只播放音频，不浪费刷新次数 */
    if (action == 0 || action == 1 || action == 2) {
        page_router_render_top();
    }
}

bool study_mode_is_revealed(void)
{
    return s_reveal;
}

void study_mode_reset_cursor(void)
{
    s_cursor = 0;
    s_reveal = true;
    LOG_D("cursor reset to #0");
    page_router_render_top();
}

bool study_mode_enter_wrongbook(void)
{
    if (learning_state_wrong_count() == 0) {
        LOG_W("wrong book empty, nothing to enter");
        return false;
    }
    s_current = MODE_WRONGBOOK;
    s_cursor = 0;
    s_reveal = true;
    LOG_I("entered wrong book (%d words)", learning_state_wrong_count());
    return true;
}

void study_mode_exit_wrongbook(void)
{
    /* 临时视图：不写 last_mode，重启后自然回学习模式 */
    s_current = MODE_FLASH;
    s_cursor = 0;
    s_reveal = true;
    LOG_I("left wrong book");
}

bool study_mode_enter_collection(void)
{
    if (learning_state_collected_count() == 0) {
        LOG_W("collection empty, nothing to enter");
        return false;
    }
    s_current = MODE_COLLECTION;
    s_cursor = 0;
    s_reveal = true;
    LOG_I("entered collection (%d words)", learning_state_collected_count());
    return true;
}

void study_mode_exit_collection(void)
{
    /* 临时视图：不写 last_mode，重启后自然回学习模式 */
    s_current = MODE_FLASH;
    s_cursor = 0;
    s_reveal = true;
    LOG_I("left collection");
}

int study_mode_enter_chat(const chat_request_t *req)
{
    /* 前置：对话全程依赖网络（上传/下载）与 SD（回复 MP3 落盘播放）；
     * 错误码供菜单层留页提示原因（2026-09-01 真机实测：静默回学习页
     * 用户无法得知拒绝原因） */
    if (!wifi_is_connected()) {
        LOG_W("chat enter rejected: no wifi");
        return 1;
    }
    if (!sync_has_device_key()) {
        LOG_W("chat enter rejected: no device key");
        return 2;
    }
    if (mkdir(AUDIO_DIR, 0775) != 0 && errno != EEXIST) {
        LOG_W("chat enter rejected: no SD (errno=%d)", errno);
        return 3;
    }
    s_current = MODE_CHAT;
    s_cursor = 0;
    s_reveal = true;
    chat_mode_enter(req);   /* 启动常驻对话任务（A1 模式透传；失败自退） */
    if (!chat_mode_is_active()) return 4;   /* 任务创建失败（内存） */
    LOG_I("entered chat mode");
    return 0;
}

void study_mode_exit_chat(void)
{
    chat_mode_request_exit();   /* 停播+置消位，任务循环边界静默收尾 */
    s_current = MODE_FLASH;
    s_cursor = 0;
    s_reveal = true;
    LOG_I("left chat mode");
}

bool study_mode_enter_quiz(void)
{
    /* 前置：词库 ≥ 8（quiz_session 干扰项来源下限）；题池构造、
     * 会话开启与首帧渲染由 main.cpp 适配层在 enter 成功后执行
     * （QUIZ_DESIGN §7） */
    if (word_parser_get_count() < 8) {
        LOG_W("quiz enter rejected: only %d words",
              word_parser_get_count());
        return false;
    }
    s_current = MODE_QUIZ;
    s_cursor = 0;
    s_reveal = true;
    LOG_I("entered quiz mode");
    return true;
}

void study_mode_exit_quiz(void)
{
    /* 临时视图：不写 last_mode；作答评分即时生效，退出无补偿 */
    s_current = MODE_FLASH;
    s_cursor = 0;
    s_reveal = true;
    LOG_I("left quiz mode");
}

/* ---- 教材目录浏览/语音查词临时视图（第五先例）+ 共用 seek ---- */

static int s_browse_prev_cursor = 0;   /* 进目录视图前的闪卡游标（退出恢复） */
static int s_voice_prev_cursor = 0;    /* 进语音视图前的闪卡游标（退出恢复） */

bool study_mode_enter_browse(void)
{
    /* 前置：词库 ≥ 1（目录索引由装载链路 catalog_build 构建，空/失配
     * 由渲染层兑底）；三级视图状态与首帧由调用方自理 */
    if (word_parser_get_count() == 0) {
        LOG_W("browse enter rejected: empty vocab");
        return false;
    }
    s_browse_prev_cursor = s_cursor;
    s_current = MODE_BROWSE;
    s_cursor = 0;
    s_reveal = true;
    LOG_I("entered browse mode");
    return true;
}

void study_mode_exit_browse(void)
{
    /* 临时视图：不写 last_mode；游标恢复进视图前的闪卡位置（浏览取消
     * 不丢学习进度）；选词跳转已走 study_mode_seek（模式已切 FLASH） */
    s_current = MODE_FLASH;
    s_cursor = s_browse_prev_cursor;
    s_reveal = true;
    LOG_I("left browse mode");
}

bool study_mode_enter_voice_search(void)
{
    /* 前置：上传查词全程依赖网络；无 SD 依赖（PSRAM 缓冲直传）。
     * 状态机复位/首帧由调用方自理（同 QUIZ 先例） */
    if (!wifi_is_connected() || !sync_has_device_key()) {
        LOG_W("voice enter rejected: wifi=%d key=%d",
              wifi_is_connected(), sync_has_device_key());
        return false;
    }
    s_voice_prev_cursor = s_cursor;
    s_current = MODE_VOICE;
    s_cursor = 0;
    s_reveal = true;
    LOG_I("entered voice search mode");
    return true;
}

void study_mode_exit_voice_search(void)
{
    s_current = MODE_FLASH;
    s_cursor = s_voice_prev_cursor;
    s_reveal = true;
    LOG_I("left voice search mode");
}

void study_mode_seek(int word_index)
{
    /* 词库索引定位（browse 选词/voice 候选确认共用）：切 FLASH +
     * 游标=index 钳位 + 渲染；不写 NVS（FLASH 本就可恢复） */
    int total = word_parser_get_count();
    if (total <= 0) return;
    if (word_index < 0) word_index = 0;
    if (word_index >= total) word_index = total - 1;
    s_current = MODE_FLASH;
    s_cursor = word_index;
    s_reveal = true;
    LOG_I("seek to word #%d", word_index);
    page_router_render_top();
}

bool study_mode_after_uncollect(void)
{
    if (s_current != MODE_COLLECTION) return false;

    /* 当前词已移出收藏序列：后词前移；清空自动退回闪卡 */
    int total = learning_state_collected_count();
    if (total == 0) {
        study_mode_exit_collection();
        return true;
    }
    /* 越界钳到 n-1：取消末词时显示前一词，不回绕跳跃
     * （与 after_quality 的回绕 0 策略略异，体验优先） */
    if (s_cursor >= total) s_cursor = total - 1;
    return true;
}

bool study_mode_after_quality(int quality)
{
    if (s_current != MODE_WRONGBOOK || quality < 3) return false;

    /* 当前词连错清零已移出错词序列：后词前移，游标原位停留 */
    if (learning_state_wrong_count() == 0) {
        study_mode_exit_wrongbook();
        return true;
    }
    if (s_cursor >= learning_state_wrong_count()) s_cursor = 0;
    return true;
}

/* 复习模式自评后：评分即置会话 done 位（该词移出到期序列），后词
 * 前移、游标钳 n-1（与 after_uncollect 同策略：取消末词显前一词）；
 * 序列清空钳 0（渲染层显「今日无到期词」空态页）。任一 quality
 * 值都出队——本会话已过，避免原地循环。
 * @return true 表示游标/序列变化，需重绘当前页。 */
bool study_mode_after_due_review(void)
{
    if (s_current != MODE_REVIEW) return false;

    int total = learning_state_due_count();
    if (s_cursor >= total) s_cursor = total > 0 ? total - 1 : 0;
    return true;
}

int study_mode_current_word_index(void)
{
    return seq_word_index(s_cursor);
}

void study_mode_reader_font_step(int dir)
{
    if (s_current != MODE_READER || !reader_ready()) return;
    s_cursor = reader_font_step(dir, s_cursor);
    page_router_render_top();   /* base READER 分支 seq_pos 同口径 */
}

int study_mode_seq_pos(void)
{
    return s_cursor;
}

int study_mode_seq_total(void)
{
    return seq_total();
}
