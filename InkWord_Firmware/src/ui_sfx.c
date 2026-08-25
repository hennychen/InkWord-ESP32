/**
 * @file ui_sfx.c
 * @brief UI 提示音实现 (v1.1 T1.6，2026-08-24)
 *
 * 样本资产链：python3 tools/gen_ui_sounds.py 生成 4 个 WAV 到
 * tools/ui_sounds/，人工拷贝到 SD 卡 /sdcard/audio/ui/。
 * 播放全走 audio_player 异步队列（<0.3s 短样本不打断学习流）。
 */
#include "ui_sfx.h"
#include "audio_player.h"
#include "gpio_config.h"       /* AUDIO_DIR */
#include "storage_manager.h"   /* storage_file_exists */
#include "settings_ui.h"       /* v1.2 T2.5：提示音门控（set_audio） */
#include "debug_log.h"

#include <stdio.h>

static const char *TAG = "SFX";

static const char *s_names[] = { "key", "rate", "mode", "err" };
static bool s_avail[sizeof(s_names) / sizeof(s_names[0])] = { false };

static void sfx_path(char *buf, size_t len, ui_sfx_t ev)
{
    snprintf(buf, len, "%s/ui/%s.wav", AUDIO_DIR, s_names[ev]);
}

void ui_sfx_init(void)
{
    char path[80];
    for (int i = 0; i < (int)(sizeof(s_avail) / sizeof(s_avail[0])); i++) {
        sfx_path(path, sizeof(path), (ui_sfx_t)i);
        s_avail[i] = storage_file_exists(path);
        if (!s_avail[i])
            LOG_W("ui sfx missing: %s", path);
    }
}

void ui_sfx_play(ui_sfx_t ev)
{
    /* v1.2 T2.5 提示音门控（set_audio；词条发音与跟读链路不归此门控，
     * 在 study_mode_machine speak 处单独拦截） */
    if (!settings_audio_enabled()) return;
    if (ev < UI_SFX_KEY || ev > UI_SFX_ERR || !s_avail[ev]) return;
    char path[80];
    sfx_path(path, sizeof(path), ev);
    audio_play_file(path);   /* 异步投递即返；正在播长样本时打断重播 */
}
