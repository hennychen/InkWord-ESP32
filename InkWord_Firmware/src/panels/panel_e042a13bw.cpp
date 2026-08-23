/**
 * @file panel_e042a13bw.cpp
 * @brief 4.2" 400x300 BW（HINK-E042A13-A0 黑白版，24Pin）面板描述符骨架
 *
 * 2026-08-23 创建：屏在途，驱动序列未 bring-up（PANEL_COMPAT_DESIGN
 * §十六 SOP 第 4 步起未走）。控制器初判 SSD1619——厂商资料称汉朔
 * Stellar-XL 价签所用 4.2" BW 屏为 SSD1619，且同 FPC 三色兄弟屏
 * （panel_e042a13_ssd1619，2026-08-22 真机实证）同为 SSD1619；
 * BW/三色仅 booster 与位平面数不同，序列大概率可大量参考，仍待实测。
 *
 * 已定字段：几何 400x300 横向原生 rotation=0（镜像三色兄弟，gfx
 * 400x300，LAYOUT_MID 档——布局层现役尺寸，UI 零改动即适配）。
 * 时序字段暂以三色兄弟实测值占位（BUSY=HIGH 忙 / 全刷 ~14.6s 量级），
 * BW 模式实际更快，bring-up 实测回填；严禁实测前驱动硬件。
 *
 * ops 为 fail-safe 桩：init 返回 -1，烧录此 env 的设备在
 * epd_driver_init 处 LOG_E 退出（屏无输出），仅供编译验证与面板轴
 * 预留；bring-up 时按 §十六 回填（序列可参考 panel_e042a13_ssd1619
 * 的 SSD1619 手写序列，去红平面、换 BW booster 配置）。
 */
#include "../epd_panel.h"

/* fail-safe 桩（参数省名防 -Wunused-parameter；驱动侧已有失败日志） */
static int stub_init(void) { return -1; }
static int stub_refresh(const uint8_t *) { return -1; }
static int stub_partial(const uint8_t *, const uint8_t *, uint8_t)
    { return -1; }

extern const epd_panel_desc_t g_panel_e042a13bw;

const epd_panel_desc_t g_panel_e042a13bw = {
    .name       = "e042a13bw_ssd1619",
    .controller = EPD_CTRL_SSD1619,  /* 初判（见文件头），bring-up 实证 */
    .panel_w    = 400,
    .panel_h    = 300,
    .gfx_rotation = 0,       /* 横向原生面板（gfx 400x300，LAYOUT_MID） */
    .color_mode = EPD_COLOR_BW,
    .plane_count = 1,        /* BW 单平面（三色兄弟为 2） */
    .palette    = {
        /* BW 语义同 panel_depg0370（§9.3/§9.4：bit0=B/W 平面白位） */
        [EPD_GFX_WHITE]  = 0x01,
        [EPD_GFX_BLACK]  = 0x00,
        [EPD_GFX_ACCENT] = 0x00,   /* BW 退化：强调色降级黑（§9.2） */
        [EPD_GFX_AUX]    = 0x00,
    },
    .fb_location = EPD_FB_AUTO,    /* 双帧+画布 ~45KB 全 SRAM */
    .rst_pulse_ms = 20,
    .busy_level = 1,               /* SSD16xx 系 HIGH 忙（兄弟屏实证） */
    .busy_timeout_ms = 20000,      /* 三色兄弟实测忙 14.6s 量级占位；
                                    * BW 模式实际更快，bring-up 回填 */
    .power_on_ms  = 40,
    .power_off_ms = 30,
    .full_ms    = 15000,           /* 占位：BW 模式实际更快，实测回填 */
    .partial_ms = 16000,           /* 占位：局刷未开前 partial==full */
    .partial_enabled = false,      /* BW SSD1619 多支持局刷，实证后再开 */
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
