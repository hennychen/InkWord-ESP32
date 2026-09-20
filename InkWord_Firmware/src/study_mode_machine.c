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
#include "chapter_index.h"   /* 2026-09-05 阅读器增强：章节跳转 */
#include "word_card_ui.h"    /* 架构拆分 2026-09-17：page 回调 ui_render_word */
#include "word_view_page.h"  /* 架构拆分 2026-09-17：page 回调 word_view_on_button */
#include "ui_sfx.h"          /* R4.2：练习模式音效 */

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

/* ---- R4.2 错词练习队列（2026-09-20） ----
 * 错词本内一键生成随机序练习队列；完成后显示汇总页。
 * 练习期间 seq_total/seq_word_index 切换到队列视图，
 * 自评质量 >=3 的词从错词本移除（learning_state 自动处理），
 * 队列内已移除词跳过。 */
#define PRACTICE_QUEUE_MAX 128
static int  s_practice_queue[PRACTICE_QUEUE_MAX];
static int  s_practice_count = 0;
static bool s_practice_active = false;
static bool s_practice_done = false;
static int  s_practice_total = 0;   /* 开始时的词数（汇总用） */

static const char *s_names[MODE_COUNT] =
    { "Flash", "Dictation", "Review", "Reader", "WrongBook", "收藏",
      "AI Chat", "测验", "目录", "语音", "墨封录" };

/* ---- 序列抽象：默认全词库，错词本换连错过滤视图，阅读换页序列 ---- */

static int seq_total(void)
{
    if (s_current == MODE_WRONGBOOK) {
        /* R4.2：练习队列激活时覆盖错词本序列 */
        if (s_practice_active) return s_practice_count > 0 ? 1 : 0;
        return learning_state_wrong_count();
    }
    if (s_current == MODE_COLLECTION) return learning_state_collected_count();
    if (s_current == MODE_MASTERED)   return learning_state_mastered_count();
    if (s_current == MODE_REVIEW)     return learning_state_due_count();
    if (s_current == MODE_READER)     return reader_page_count();
    if (s_current == MODE_CHAT)       return 0;  /* 对话无词序列（状态栏 0/0） */
    if (s_current == MODE_QUIZ)       return 0;  /* 测验题号由 main.cpp 自绘状态栏 */
    if (s_current == MODE_BROWSE)     return 0;  /* 目录页码由 browse_mode 自绘 */
    if (s_current == MODE_VOICE)      return 0;  /* 候选列表由 voice_search 自绘 */
    /* 闪卡/听写：未墨封视图（2026-09-04 墨封——学习主链路过滤，
     * 默认全库直映射退役；无墨封词时与旧行为等价） */
    return learning_state_active_count();
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
    if (s_current == MODE_WRONGBOOK) {
        /* R4.2：练习队列激活时从队列取词 */
        if (s_practice_active && s_practice_count > 0)
            return s_practice_queue[0];
        return learning_state_wrong_at(cursor);
    }
    if (s_current == MODE_COLLECTION) return learning_state_collected_at(cursor);
    if (s_current == MODE_MASTERED)   return learning_state_mastered_at(cursor);
    if (s_current == MODE_REVIEW)     return learning_state_due_at(cursor);
    /* 闪卡/听写：active 视图虚游走（越界 -1 交调用方空词兑底） */
    return learning_state_active_at(cursor);
}

void study_mode_init(void)
{
    /* 从 NVS 恢复上次模式（错词本/收藏/墨封录/对话是临时视图，不接受恢复） */
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t m = 0;
        if (nvs_get_u8(h, NVS_KEY_LAST_MODE, &m) == ESP_OK &&
            m < MODE_COUNT && m != MODE_WRONGBOOK && m != MODE_COLLECTION &&
            m != MODE_MASTERED &&
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

    /* R2.2：进入听写模式时重置会话统计 */
    if (s_current == MODE_DICTATION) {
        dictation_session_reset();
    }

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
             s_current == MODE_MASTERED ||
             s_current == MODE_CHAT || s_current == MODE_QUIZ ||
             s_current == MODE_BROWSE || s_current == MODE_VOICE);
    apply_mode(s_current);
    return s_current;
}

void study_mode_set(study_mode_t mode)
{
    if (mode < 0 || mode >= MODE_COUNT) return;
    if (mode == MODE_WRONGBOOK || mode == MODE_COLLECTION ||
        mode == MODE_MASTERED ||
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

/* ---- 墨封录临时视图（2026-09-04，收藏浏览同构镜像） ---- */

bool study_mode_enter_mastered(void)
{
    if (learning_state_mastered_count() == 0) {
        LOG_W("mastered list empty, nothing to enter");
        return false;
    }
    s_current = MODE_MASTERED;
    s_cursor = 0;
    s_reveal = true;
    LOG_I("entered mastered list (%d words)", learning_state_mastered_count());
    return true;
}

void study_mode_exit_mastered(void)
{
    /* 临时视图：不写 last_mode，重启后自然回学习模式 */
    s_current = MODE_FLASH;
    s_cursor = 0;
    s_reveal = true;
    LOG_I("left mastered list");
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
     * 游标=index 钳位 + 渲染；不写 NVS（FLASH 本就可恢复）。
     * 2026-09-04 墨封：目标词已墨封时先启封——目录/语音选词=要学它
     * （跳转即启封语义；不启封则 active 视图反查不到该词） */
    int total = word_parser_get_count();
    if (total <= 0) return;
    if (word_index < 0) word_index = 0;
    if (word_index >= total) word_index = total - 1;
    if (learning_state_is_mastered(word_index)) {
        learning_state_toggle_master(word_index);
        LOG_I("seek unmastered word #%d", word_index);
    }
    s_current = MODE_FLASH;
    /* 词库索引→active 序列位置反查（O(N) 单次，翻词路径同量级） */
    s_cursor = 0;
    int pos = 0;
    for (int i = 0; i <= word_index; i++)
        if (!learning_state_is_mastered(i)) pos++;
    s_cursor = pos - 1;
    s_reveal = true;
    LOG_I("seek to word #%d (active pos %d)", word_index, s_cursor);
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

/* 墨封/启封后的序列收缩钳位（2026-09-04）：当前词移出所在序列后
 * 后词前移；游标钳位各模式跟随既有先例（闪卡/听写=after_uncollect
 * 钳 n-1、错词本=after_quality 回绕 0、复习=after_due_review 钳
 * total>0?n-1:0）；错词本/墨封录清空自动退回闪卡；闪卡全库墨封完
 * 鉗 0 交渲染层显「全部词已墨封」空态页。
 * @return true 表示游标/模式变化，需重绘当前页。 */
bool study_mode_after_master(void)
{
    if (s_current == MODE_FLASH || s_current == MODE_DICTATION) {
        int total = learning_state_active_count();
        if (total == 0) { s_cursor = 0; return true; }
        if (s_cursor >= total) s_cursor = total - 1;
        return true;
    }
    if (s_current == MODE_WRONGBOOK) {
        if (learning_state_wrong_count() == 0) {
            study_mode_exit_wrongbook();
            return true;
        }
        if (s_cursor >= learning_state_wrong_count()) s_cursor = 0;
        return true;
    }
    if (s_current == MODE_REVIEW) {
        int total = learning_state_due_count();
        if (s_cursor >= total) s_cursor = total > 0 ? total - 1 : 0;
        return true;
    }
    if (s_current == MODE_MASTERED) {
        int total = learning_state_mastered_count();
        if (total == 0) {
            study_mode_exit_mastered();
            return true;
        }
        if (s_cursor >= total) s_cursor = total - 1;
        return true;
    }
    return false;
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

void study_mode_reader_chapter_step(int dir)
{
    if (s_current != MODE_READER || !reader_ready()) return;
    int ch_count = chapter_index_count();
    if (ch_count <= 0) return;
    int cur_ch = chapter_index_find_by_page(s_cursor);
    int target_ch = cur_ch + dir;
    if (target_ch < 0 || target_ch >= ch_count) {
        haptic_event(HAPTIC_ERROR);   /* 首/末章边界拒绝 */
        return;
    }
    s_cursor = chapter_index_jump_to(target_ch);
    page_router_render_top();
}

void study_mode_reader_goto_page(int page)
{
    if (s_current != MODE_READER || !reader_ready()) return;
    int total = reader_page_count();
    if (page < 0) page = 0;
    if (page >= total) page = total - 1;
    s_cursor = page;
    page_router_render_top();
}

int study_mode_seq_pos(void)
{
    return s_cursor;
}

int study_mode_seq_total(void)
{
    return seq_total();
}

/* ================================================================
 * R4.2 错词练习队列（2026-09-20）
 * 错词本内一键生成随机序练习队列；完成后显示汇总页。
 * ================================================================ */

/* Fisher-Yates 洗牌（练习队列随机序） */
static void practice_shuffle(void)
{
    for (int i = s_practice_count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = s_practice_queue[i];
        s_practice_queue[i] = s_practice_queue[j];
        s_practice_queue[j] = tmp;
    }
}

/* 启动练习：收集错词到队列并洗牌 */
bool study_mode_start_practice(void)
{
    int wrong_count = learning_state_wrong_count();
    if (wrong_count <= 0) return false;

    s_practice_count = (wrong_count < PRACTICE_QUEUE_MAX)
                       ? wrong_count : PRACTICE_QUEUE_MAX;
    for (int i = 0; i < s_practice_count; i++) {
        s_practice_queue[i] = learning_state_wrong_at(i);
    }
    practice_shuffle();
    s_practice_active = true;
    s_practice_done = false;
    s_practice_total = s_practice_count;
    s_cursor = 0;
    s_reveal = true;
    LOG_I("practice started: %d words", s_practice_count);
    return true;
}

/* 停止练习：清空队列 */
void study_mode_stop_practice(void)
{
    s_practice_active = false;
    s_practice_done = false;
    s_practice_count = 0;
    s_practice_total = 0;
    LOG_I("practice stopped");
}

/* 练习完成：标记完成并切换到汇总页 */
static void practice_mark_done(void)
{
    s_practice_done = true;
    s_practice_active = false;
    LOG_I("practice completed: %d words", s_practice_total);
}

/* 练习队列前进到下一个有效词（跳过已从错词本移除的词） */
static bool practice_advance(void)
{
    if (!s_practice_active || s_practice_count <= 0) return false;

    /* 当前词在队列头部，移除（无论是否答对，都移到下一个） */
    for (int i = 0; i < s_practice_count - 1; i++) {
        s_practice_queue[i] = s_practice_queue[i + 1];
    }
    s_practice_count--;

    if (s_practice_count <= 0) {
        practice_mark_done();
        return false;
    }
    return true;
}

bool study_mode_practice_is_active(void)
{
    return s_practice_active || s_practice_done;
}

int study_mode_practice_done_count(void)
{
    return s_practice_total - s_practice_count;
}

int study_mode_practice_total(void)
{
    return s_practice_total;
}

/* ---- 练习完成汇总页 ---- */
static void practice_summary_render(void)
{
    ui_render_practice_summary();   /* word_card_ui.cpp 实现（整屏全刷） */
}

static void practice_summary_enter(void)
{
    practice_summary_render();
}

static bool practice_summary_on_button(nav_key_t id, button_event_t event)
{
    if (event != BUTTON_EVENT_SHORT_PRESS && event != BUTTON_EVENT_LONG_PRESS)
        return true;
    /* 任意键退出汇总页 */
    study_mode_stop_practice();
    page_router_exit(&g_practice_summary_page);
    page_router_render_top();
    return true;
}

const page_t g_practice_summary_page = {
    "practice_summary", practice_summary_render,
    practice_summary_on_button, practice_summary_enter,
    NULL, true
};

/* ================================================================
 * 学习视图栈页 page_t 定义（架构拆分 2026-09-17，自 main.cpp 迁入）
 * owns_display=false 复用渲染族（ui_render_word）；on_button 委托
 * word_view_on_button（base + 栈页同源），返回 false 时 dispatch
 * 统一 pop+render_top 回上级。
 * ================================================================ */

/* ---- 错词本 ---- */
static void wrongbook_render(void)
{
    ui_render_word(MODE_WRONGBOOK, study_mode_current_word_index());
}

static void wrongbook_enter(void)
{
    wrongbook_render();   /* 状态由调用方 enter_wrongbook 先置（预检） */
}

static void wrongbook_exit(void)
{
    study_mode_exit_wrongbook();
}

static bool wrongbook_on_button(nav_key_t id, button_event_t event)
{
    /* R4.2：练习模式按键处理 */
    if (s_practice_done) {
        /* 练习完成：任意键进汇总页 */
        if (event == BUTTON_EVENT_SHORT_PRESS || event == BUTTON_EVENT_LONG_PRESS) {
            page_router_push(&g_practice_summary_page);
        }
        return true;
    }
    if (s_practice_active) {
        /* 练习进行中 */
        if (event == BUTTON_EVENT_LONG_PRESS) {
            if (id == NAV_UP) {
                /* 上键长按：退出练习 */
                study_mode_stop_practice();
                page_router_render_top();
                return true;
            }
            /* 其他长按委托给 word_view_on_button（菜单/清屏等） */
            return word_view_on_button(MODE_WRONGBOOK, id, event);
        }
        if (event == BUTTON_EVENT_SHORT_PRESS) {
            if (id == NAV_LEFT || id == NAV_RIGHT) {
                /* 左/右：自评 + 队列前进 */
                int quality = (id == NAV_LEFT) ? 1 : 5;
                int word_idx = seq_word_index(s_cursor);
                dictation_session_record(quality);
                learning_state_apply_quality(word_idx, quality);
                haptic_event(HAPTIC_REVIEW);
                ui_sfx_play(UI_SFX_RATE);
                practice_advance();
                if (s_practice_done) {
                    page_router_push(&g_practice_summary_page);
                } else {
                    page_router_render_top();
                }
                return true;
            }
            /* 上下翻词：练习模式禁止（一次一题） */
            if (id == NAV_UP || id == NAV_DOWN) return true;
        }
        /* 其他键（中=发音，SET=揭晓）委托 */
        return word_view_on_button(MODE_WRONGBOOK, id, event);
    }
    /* 非练习模式：上键长按启动练习 */
    if (event == BUTTON_EVENT_LONG_PRESS && id == NAV_UP) {
        if (study_mode_start_practice()) {
            haptic_event(HAPTIC_MODE);
            ui_sfx_play(UI_SFX_MODE);
            page_router_render_top();
        } else {
            haptic_event(HAPTIC_ERROR);
            ui_sfx_play(UI_SFX_ERR);
        }
        return true;
    }
    return word_view_on_button(MODE_WRONGBOOK, id, event);
}

const page_t g_wrongbook_page = { "wrongbook", wrongbook_render,
                                  wrongbook_on_button, wrongbook_enter,
                                  wrongbook_exit, false };

/* ---- 收藏浏览 ---- */
static void collection_render(void)
{
    ui_render_word(MODE_COLLECTION, study_mode_current_word_index());
}

static void collection_enter(void)
{
    study_mode_enter_collection();   /* 调用方计数预检非零必成功 */
    collection_render();             /* 首帧（ui_render_word 模式变化全刷） */
}

static void collection_exit(void)
{
    study_mode_exit_collection();
}

static bool collection_on_button(nav_key_t id, button_event_t event)
{
    return word_view_on_button(MODE_COLLECTION, id, event);
}

const page_t g_collection_page = { "collection", collection_render,
                                   collection_on_button,
                                   collection_enter,
                                   collection_exit, false };

/* ---- 墨封录 ---- */
static void mastered_render(void)
{
    ui_render_word(MODE_MASTERED, study_mode_current_word_index());
}

static void mastered_enter(void)
{
    study_mode_enter_mastered();   /* act_collection 同构（预检必成功） */
    mastered_render();
}

static void mastered_exit(void)
{
    study_mode_exit_mastered();
}

static bool mastered_on_button(nav_key_t id, button_event_t event)
{
    return word_view_on_button(MODE_MASTERED, id, event);
}

const page_t g_mastered_page = { "mastered", mastered_render,
                                 mastered_on_button,
                                 mastered_enter, mastered_exit,
                                 false };

/* ---- 听写汇总栈页（R2.2，2026-09-20）：切离听写时显示会话统计 ---- */
static void dictation_summary_render(void)
{
    ui_render_dictation_summary();   /* word_card_ui.cpp 实现（整屏全刷） */
}

static void dictation_summary_enter(void)
{
    /* 入栈即切换模式（会话数据在 apply_mode 切离 DICTATION 时不重置，
     * 仅进入 DICTATION 时重置；ui_render_dictation_summary 读 session） */
    study_mode_switch_next();
    dictation_summary_render();      /* 首帧自绘 */
}

static bool dictation_summary_on_button(nav_key_t id, button_event_t event)
{
    (void)id;
    if (event != BUTTON_EVENT_SHORT_PRESS &&
        event != BUTTON_EVENT_LONG_PRESS) return true;
    /* 任意键退出汇总页，render_top 回 base 分流新模式 */
    page_router_exit(&g_dictation_summary_page);
    page_router_render_top();
    return true;
}

const page_t g_dictation_summary_page = {
    "dict_summary", dictation_summary_render,
    dictation_summary_on_button, dictation_summary_enter,
    NULL, true   /* owns_display：汇总页独占整帧 */
};

/* ---- 听写会话统计（R2.2，2026-09-20）---- */
static dictation_session_t s_dict_session = {0, 0, 0};

void dictation_session_reset(void)
{
    s_dict_session.total = 0;
    s_dict_session.correct = 0;
    s_dict_session.wrong = 0;
    LOG_I("dictation session reset");
}

void dictation_session_record(int quality)
{
    if (s_current != MODE_DICTATION) return;
    s_dict_session.total++;
    if (quality >= 3) {
        s_dict_session.correct++;
    } else {
        s_dict_session.wrong++;
    }
    LOG_I("dictation record: q=%d total=%d correct=%d wrong=%d",
          quality, s_dict_session.total, s_dict_session.correct, s_dict_session.wrong);
}

dictation_session_t dictation_session_get(void)
{
    return s_dict_session;
}

bool dictation_session_has_data(void)
{
    return s_dict_session.total > 0;
}
