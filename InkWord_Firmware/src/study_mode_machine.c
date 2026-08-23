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
#include "word_parser.h"
#include "learning_state.h"
#include "reader_engine.h"   /* READER 模式：页序列/字号切换/进度恢复 */

#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "MODE";

static study_mode_t s_current = MODE_FLASH;

/* 当前词索引（各模式独立游标演示，实际可扩展为独立游标） */
static int s_cursor = 0;

/* 释义显示状态：true=显示（默认，与历史行为一致），false=遮蔽（自测）；
 * SET 翻义切换；翻页/切模式自动回显示（新词全显，按 SET 开始遮蔽自测） */
static bool s_reveal = true;

static const char *s_names[MODE_COUNT] =
    { "Flash", "Dictation", "Review", "Reader", "WrongBook", "收藏" };

/* ---- 序列抽象：默认全词库，错词本换连错过滤视图，阅读换页序列 ---- */

static int seq_total(void)
{
    if (s_current == MODE_WRONGBOOK)  return learning_state_wrong_count();
    if (s_current == MODE_COLLECTION) return learning_state_collected_count();
    if (s_current == MODE_READER)     return reader_page_count();
    return word_parser_get_count();
}

/* 进入阅读模式时恢复上次阅读页（书签名匹配才有效，否则回第 0 页） */
static void reader_cursor_restore(void)
{
    s_cursor = 0;
    int p = reader_progress_page();
    if (p >= 0) s_cursor = p;
}

/* 游标 -> 词库索引（错词本/收藏浏览模式下为过滤序列内第 cursor 个词） */
static int seq_word_index(int cursor)
{
    if (s_current == MODE_WRONGBOOK)  return learning_state_wrong_at(cursor);
    if (s_current == MODE_COLLECTION) return learning_state_collected_at(cursor);
    return cursor;
}

void study_mode_init(void)
{
    /* 从 NVS 恢复上次模式（错词本是临时视图，不接受恢复） */
    nvs_handle_t h;
    if (nvs_open("inkword", NVS_READONLY, &h) == ESP_OK) {
        uint8_t m = 0;
        if (nvs_get_u8(h, "last_mode", &m) == ESP_OK &&
            m < MODE_COUNT && m != MODE_WRONGBOOK && m != MODE_COLLECTION) {
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
    if (nvs_open("inkword", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "last_mode", (uint8_t)s_current);
        nvs_commit(h);
        nvs_close(h);
    }

    LOG_I("mode switched -> %s", s_names[s_current]);
}

study_mode_t study_mode_switch_next(void)
{
    /* 跳过两个临时视图（do-while 保证至少前进一步，不会死循环） */
    do {
        s_current = (study_mode_t)((s_current + 1) % MODE_COUNT);
    } while (s_current == MODE_WRONGBOOK || s_current == MODE_COLLECTION);
    apply_mode(s_current);
    return s_current;
}

void study_mode_set(study_mode_t mode)
{
    if (mode < 0 || mode >= MODE_COUNT) return;
    if (mode == MODE_WRONGBOOK || mode == MODE_COLLECTION) return;
    apply_mode(mode);   /* 同模式重入也归零游标，与 switch_next 语义一致 */
}

const char *study_mode_name(study_mode_t mode)
{
    return (mode >= 0 && mode < MODE_COUNT) ? s_names[mode] : "Unknown";
}

/* ---- 模式相关渲染桩：实际由 UI 渲染模块填充 ---- */
extern void ui_render_word(study_mode_t mode, int index);  /* 定义在 main.c */

/* ---- 语义动作处理 ---- */
void study_mode_handle_action(int action)
{
    int total = seq_total();

    switch (action) {
    case 0: /* prev */
        if (total <= 0) return;         /* 空序列（READER 无书）不动作 */
        if (--s_cursor < 0) s_cursor = total - 1;
        s_reveal = true;
        break;
    case 1: /* next */
        if (total <= 0) return;
        if (++s_cursor >= total) s_cursor = 0;
        s_reveal = true;
        break;
    case 2: /* confirm：闪卡模式翻转释义，听写模式提交拼写 */
        if (s_current == MODE_READER) return;  /* 书页无遮蔽语义 */
        s_reveal = !s_reveal;
        LOG_D("confirm action in %s mode (reveal=%d)",
              s_names[s_current], s_reveal);
        break;
    case 3: { /* speak：播放当前词音频 */
        if (s_current == MODE_READER) return;  /* 书页无词音频 */
        const WordEntry *w = word_parser_get(seq_word_index(s_cursor));
        if (w && w->audio[0]) {
            char path[128];
            snprintf(path, sizeof(path), "%s/%s", AUDIO_DIR, w->audio);
            if (audio_play_file(path) != 0) {
                /* TEMP 2026-08-23 SD 卡未挂载时降级为测试音（验证后移除） */
                LOG_W("audio file not found, playing test tone");
                extern void audio_play_test_tone(void);
                audio_play_test_tone();
            }
        } else {
            /* TEMP 2026-08-23 无音频字段时播放测试音（验证后移除） */
            LOG_W("no audio field, playing test tone");
            extern void audio_play_test_tone(void);
            audio_play_test_tone();
        }
        break;
    }
    default:
        break;
    }

    /* 仅画面变化的动作触发重绘：翻页(prev/next)与翻义(confirm) 局刷内容区；
     * speak(3) 只播放音频，不浪费刷新次数 */
    if (action == 0 || action == 1 || action == 2) {
        ui_render_word(s_current, seq_word_index(s_cursor));
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
    ui_render_word(s_current, seq_word_index(s_cursor));
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

int study_mode_current_word_index(void)
{
    return seq_word_index(s_cursor);
}

void study_mode_reader_font_step(int dir)
{
    if (s_current != MODE_READER || !reader_ready()) return;
    s_cursor = reader_font_step(dir, s_cursor);
    ui_render_word(s_current, s_cursor);
}

int study_mode_seq_pos(void)
{
    return s_cursor;
}

int study_mode_seq_total(void)
{
    return seq_total();
}
