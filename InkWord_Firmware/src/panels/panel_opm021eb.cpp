/**
 * @file panel_opm021eb.cpp
 * @brief 2.13" 122x250 BW（OPM021EB，电子标签）面板描述符骨架
 *
 * 2026-08-23 创建：屏在途，驱动序列未 bring-up（PANEL_COMPAT_DESIGN
 * §十六 SOP 第 4 步起未走）。122x250 紧凑 2.13" 电子标签常见主控
 * SSD1680（微雪 2.13 V4 同规格），待实测确认后回填 controller 与序列。
 *
 * 已定字段：几何 122x250 竖屏 rotation=0（2026-08-23 用户选型标签屏
 * 竖持 → gfx 122x250，LAYOUT_TINY 档，当前最小屏）；GFX/布局/UI 层
 * 已就绪（学习页 3 字/行×7 行、待机引文 16px 紧排版）。
 * 未定字段（时序/刷新策略）全部占位，严禁实测前填真值驱动硬件。
 *
 * ops 为 fail-safe 桩：init 返回 -1，烧录此 env 的设备在
 * epd_driver_init 处 LOG_E 退出（屏无输出），仅供编译验证与面板轴
 * 预留；bring-up 时按 §十六 回填（同 panel_wft0290 骨架先例）。
 */
#include "../epd_panel.h"

/* fail-safe 桩（参数省名防 -Wunused-parameter；驱动侧已有失败日志） */
static int stub_init(void) { return -1; }
static int stub_refresh(const uint8_t *) { return -1; }
static int stub_partial(const uint8_t *, const uint8_t *, uint8_t)
    { return -1; }

extern const epd_panel_desc_t g_panel_opm021eb;

const epd_panel_desc_t g_panel_opm021eb = {
    .name       = "opm021eb_bw",
    .controller = EPD_CTRL_UNKNOWN,  /* bring-up 实测回填（SSD1680 候选） */
    .panel_w    = 122,
    .panel_h    = 250,
    .gfx_rotation = 0,       /* 竖屏持机（gfx 122x250，LAYOUT_TINY） */
    .color_mode = EPD_COLOR_BW,
    .plane_count = 1,
    .palette    = {
        /* BW 语义同 panel_depg0370（§9.3/§9.4：bit0=B/W 平面白位） */
        [EPD_GFX_WHITE]  = 0x01,
        [EPD_GFX_BLACK]  = 0x00,
        [EPD_GFX_ACCENT] = 0x00,   /* BW 退化：强调色降级黑（§9.2） */
        [EPD_GFX_AUX]    = 0x00,
    },
    .fb_location = EPD_FB_AUTO,    /* 双帧+画布 ~11KB 全 SRAM */
    .rst_pulse_ms = 20,            /* 驱动板复位脉宽现值占位 */
    .busy_level = 0,               /* 占位：SSD1680 系应为 HIGH 忙 */
    .busy_timeout_ms = 0,          /* 占位：bring-up 实测回填 */
    .power_on_ms  = 0,
    .power_off_ms = 0,
    .full_ms    = 0,
    .partial_ms = 0,
    .partial_enabled = false,      /* bring-up 实证后再开 */
    .passes     = 1,
    .partial_count_full_refresh = 8,
    .window_8align = true,         /* 占位：以实测窗口行为为准 */
    .ops = {
        .init         = stub_init,
        .full_refresh = stub_refresh,
        .write_full   = stub_refresh,
        .partial      = stub_partial,
        .probe        = NULL,
        .write_planes = NULL,
    },
};
