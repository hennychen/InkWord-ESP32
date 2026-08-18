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

#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "MODE";

static study_mode_t s_current = MODE_FLASH;

/* 当前词索引（各模式独立游标演示，实际可扩展为独立游标） */
static int s_cursor = 0;

/* 释义显示状态：true=显示（默认，与历史行为一致），false=遮蔽（自测）；
 * SET 翻义切换；翻页/切模式自动回显示（新词全显，按 SET 开始遮蔽自测） */
static bool s_reveal = true;

static const char *s_names[MODE_COUNT] = { "Flash", "Dictation", "Review" };

void study_mode_init(void)
{
    /* 从 NVS 恢复上次模式 */
    nvs_handle_t h;
    if (nvs_open("inkword", NVS_READONLY, &h) == ESP_OK) {
        uint8_t m = 0;
        if (nvs_get_u8(h, "last_mode", &m) == ESP_OK && m < MODE_COUNT) {
            s_current = (study_mode_t)m;
        }
        nvs_close(h);
    }
    s_cursor = 0;
    LOG_I("study mode initialized: %s", s_names[s_current]);
}

study_mode_t study_mode_current(void)
{
    return s_current;
}

study_mode_t study_mode_switch_next(void)
{
    s_current = (study_mode_t)((s_current + 1) % MODE_COUNT);
    s_cursor = 0;
    s_reveal = true;

    /* 持久化 */
    nvs_handle_t h;
    if (nvs_open("inkword", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "last_mode", (uint8_t)s_current);
        nvs_commit(h);
        nvs_close(h);
    }

    LOG_I("mode switched -> %s", s_names[s_current]);
    return s_current;
}

void study_mode_set(study_mode_t mode)
{
    if (mode < 0 || mode >= MODE_COUNT) return;
    s_current = mode;
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
    int total = word_parser_get_count();

    switch (action) {
    case 0: /* prev */
        if (--s_cursor < 0) s_cursor = total - 1;
        s_reveal = true;
        break;
    case 1: /* next */
        if (++s_cursor >= total) s_cursor = 0;
        s_reveal = true;
        break;
    case 2: /* confirm：闪卡模式翻转释义，听写模式提交拼写 */
        s_reveal = !s_reveal;
        LOG_D("confirm action in %s mode (reveal=%d)",
              s_names[s_current], s_reveal);
        break;
    case 3: { /* speak：播放当前词音频 */
        const WordEntry *w = word_parser_get(s_cursor);
        if (w && w->audio[0]) {
            char path[128];
            snprintf(path, sizeof(path), "%s/%s", AUDIO_DIR, w->audio);
            audio_play_file(path);
        }
        break;
    }
    default:
        break;
    }

    /* 仅画面变化的动作触发重绘：翻页(prev/next)与翻义(confirm) 局刷内容区；
     * speak(3) 只播放音频，不浪费刷新次数 */
    if (action == 0 || action == 1 || action == 2) {
        ui_render_word(s_current, s_cursor);
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
    ui_render_word(s_current, s_cursor);
}
