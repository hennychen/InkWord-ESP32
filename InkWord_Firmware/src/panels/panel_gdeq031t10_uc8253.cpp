/**
 * @file panel_gdeq031t10_uc8253.cpp
 * @brief 3.1" 面板单元（L0）—— GxEPD2_gdeq031t10 包装 + desc 注册
 *
 * 面板：GDEQ031T10 3.1" 240x320 BW，UC8253 COG，24P FPC 0.5mm
 * 骨架取自 panel_depg0370_uc8253.cpp（同 UC8253 控制器族）。
 *
 * 规格：
 *   分辨率 240x320（竖屏原生），SPI，黑白
 *   全刷 3s / 快刷 1s / 局刷 0.5s
 *   视域 62.72x47.04mm，对角 ≈129 PPI
 *   规格建议：快刷/局刷连续 5 次后加一次全屏刷新减少残影
 *
 * 与 DEPG0370 的关键差异（demo 实证 2026-09-05）：
 *   - PSR(0x00) 仅 1 字节 0x1F（LUT from register），非 DEPG0370 的 2 字节
 *   - 局刷 E5=0x79（DEPG0370 用 100/0x64）
 *   - 快刷 E5=0x5A（DEPG0370 无此模式）
 *   - SPI 10MHz（DEPG0370 用 20MHz）
 */
#include "../epd_panel.h"
#include "../gpio_config.h"
#include "epd_bus.h"

#include <Arduino.h>
#include "../GxEPD2_gdeq031t10.h"
#include "uc8253_ops.h"   /* P1-b①：族通用 ops 宏（序列语义/调优史见该头） */

/* 前置声明：ops 实现引用 desc 几何字段 */
extern const epd_panel_desc_t g_panel_gdeq031t10;

/* epd2 层驱动对象。六个 ops 函数经 UC8253_DEFINE_OPS 宏展开
 * （P1-b①，2026-09-05：与 DEPG0370 逐字等价的包装去重；本屏帧
 * 240×320/8 = 9600B，局刷双 RAM 传帧 SPI @10MHz ≈ 10ms） */
static GxEPD2_gdeq031t10 s_epd2(
    EPD_CS_PIN, EPD_DC_PIN, EPD_RESET_PIN, EPD_BUSY_PIN);

UC8253_DEFINE_OPS(s_epd2, g_panel_gdeq031t10)

/* —— desc 注册 ——
 * 时序取 GxEPD2_gdeq031t10.h 静态属性 + 规格书标称值；
 * 阈值暂取 8（与 DEPG0370 默认一致，真机 bring-up 可按残影表现调整） */
const epd_panel_desc_t g_panel_gdeq031t10 = {
    .name       = "gdeq031t10_uc8253",
    .controller = EPD_CTRL_UC8253,
    .panel_w    = 240,
    .panel_h    = 320,
    .gfx_rotation = 1,       /* 横屏持机（gfx 320x240），bring-up 需验证方向 */
    .dpi         = 129,       /* 对角 PPI：sqrt(240²+320²)/3.1" ≈ 129 */
    .color_mode = EPD_COLOR_BW,
    .plane_count = 1,
    .palette    = {
        [EPD_GFX_WHITE]  = 0x01,
        [EPD_GFX_BLACK]  = 0x00,
        [EPD_GFX_ACCENT] = 0x00,   /* BW 退化：强调色降级黑 */
        [EPD_GFX_AUX]    = 0x00,   /* BW 退化：次强调色同降级黑 */
    },
    .accent_rgb  = 0,
    .fb_location = EPD_FB_AUTO,    /* 9.4KB 双帧+画布全 SRAM */
    .rst_pulse_ms = 20,
    .busy_level = 0,               /* BUSY=LOW 忙（demo 实证：while(!isEPD_W21_BUSY)） */
    .busy_timeout_ms = 5000,       /* 全刷 3s 规格，留 5s 余量 */
    .power_on_ms  = 50,
    .power_off_ms = 50,
    .full_ms    = 3000,            /* 规格 3s */
    .partial_ms = 500,             /* 规格 0.5s */
    .partial_enabled = true,       /* 双 RAM 差分局刷 */
    .passes     = 2,               /* 默认双刷 */
    .partial_count_full_refresh = 8, /* 暂取 8（规格建议 5 次后全刷保养） */
    .window_8align = true,
    .ops = {
        .init         = panel_init,
        .full_refresh = panel_full_refresh,
        .write_full   = panel_write_full,
        .partial      = panel_partial,
        .power_off    = panel_power_off,
        .deep_sleep   = panel_deep_sleep,
        .probe        = NULL,
        .write_planes = NULL,
        .diag         = bus_diag_uc,  /* UC8253 族 FLG 0x71 双读 */
    },
};
