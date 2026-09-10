/**
 * @file panel_depg0370_uc8253.cpp
 * @brief DEPG0370 面板单元（L0）—— GxEPD2_374_DEPG0370 包装 + desc 注册
 *
 * 面板：DEPG0370BBU253F33HP-M7 3.7"（240x416 BW，UC8253 类 COG，24P FPC）
 * 本文件是 L2 首个注册单元（PANEL_COMPAT_DESIGN §5.3，Phase 1 纯重构）：
 * 自 epd_driver.cpp 迁入面板类实例与全部面板专属刷新序列，epd_driver
 * （L3）经 desc.ops 调用，不再触碰面板类（铁律 3）。
 *
 * demo 序列映射（附录 A）：Epaper_Initial_full/partial_mode → initFullDemo/
 * initPartialDemo；EPD_Dis_Part_RAM → demoWriteFull / demoWriteDualNoWindow；
 * Epaper_Update_partial → updateDemoPartial；Epaper_READBUS/GPIO 序列 →
 * hwReset。屏厂 demo 是时序问题的最终仲裁（DEPG0370 BUSY 恒低先例）。
 */
#include "../epd_panel.h"
#include "../gpio_config.h"
#include "epd_bus.h"    /* T1.8：ops.diag 族标准实现 */

#include <Arduino.h>
#include "../GxEPD2_374_DEPG0370.h"
#include "uc8253_ops.h"   /* P1-b①：族通用 ops 宏（序列语义/调优史见该头） */

/* 前置声明：ops 实现引用 desc 几何字段（write_full 全屏参数） */
extern const epd_panel_desc_t g_panel_depg0370;

/* epd2 层驱动对象（epd2 直驱，不用 GxEPD2_BW 显示层）—— 全工程唯一样例
 * 化点，自 epd_driver.cpp 迁入（Phase 1，行为不变）。
 * 六个 ops 函数经 UC8253_DEFINE_OPS 宏展开（P1-b①，2026-09-05：与
 * 3.1" 屏逐字等价的包装去重，函数名/序列字节不变） */
static GxEPD2_374_DEPG0370 s_epd2(
    EPD_CS_PIN, EPD_DC_PIN, EPD_RESET_PIN, EPD_BUSY_PIN);

UC8253_DEFINE_OPS(s_epd2, g_panel_depg0370)

/* —— desc 注册（首个面板单元，字段值实证见各注释）——
 * 时序取 GxEPD2_374_DEPG0370.h L34-37 静态属性（power_on 50 / power_off
 * 50 / full 1500 / partial 350ms）；阈值 8 为 main.cpp 局刷计数全刷阈值
 * 现值（待机 12 / 学习·阅读 8 分页差异化由调用方设置） */
const epd_panel_desc_t g_panel_depg0370 = {
    .name       = "depg0370_uc8253",
    .controller = EPD_CTRL_UC8253,
    .otp_signature = 0,           /* 待实测指纹（自动识别暂不启用） */
    .panel_w    = 240,
    .panel_h    = 416,
    .gfx_rotation = 1,       /* 横屏持机（gfx 416x240），Phase 2 转置泛化依据 */
    .dpi         = 130,       /* 对角 PPI（诊断字段：3.7" 对角 480px；换算 mm=px÷PPI×25.4） */
    .color_mode = EPD_COLOR_BW,
    .plane_count = 1,
    .palette    = {
        /* 逻辑色 → 平面置位掩码（与 panel_e042a13 语义统一，§9.3/§9.4）：
         * bit0 = B/W 平面白位，bit1 = 红平面红位（BW 面板无 bit1）。
         * 注：色彩路由现由 epd_driver bw/ac_layer_color 实现，本表为
         * 语义权威（L4+ 查询预留） */
        [EPD_GFX_WHITE]  = 0x01,   /* bit0 置位 = 白 */
        [EPD_GFX_BLACK]  = 0x00,
        [EPD_GFX_ACCENT] = 0x00,   /* BW 退化：强调色降级黑（§9.2） */
        [EPD_GFX_AUX]    = 0x00,   /* BW 退化：次强调色同降级黑 */
        /* [4..15] 保留档位零初始化 */
    },
    .accent_rgb  = 0,                    /* BW 面板无第三色（显式零，
                                    * 补齐声明序消 -Wmissing-field-initializers） */
    .fb_location = EPD_FB_AUTO,    /* 37.4KB 双帧+画布全 SRAM（§10.1 预算表） */
    .rst_pulse_ms = 20,            /* epd_driver_init 复位脉宽现值 */
    .busy_level = 0,               /* BUSY=LOW 忙（空闲电平 1） */
    .busy_timeout_ms = 3000,
    .power_on_ms  = 50,
    .power_off_ms = 50,
    .full_ms    = 1500,
    .partial_ms = 350,
    .partial_enabled = true,       /* 双 RAM 差分局刷在用（§5.2 实证） */
    .passes     = 2,               /* epd_gfx_flush_window 默认双刷 */
    .partial_count_full_refresh = 8,
    .window_8align = true,         /* epd2 层窗口 8 对齐（gfx 侧 y/h 对齐约束） */
    .ops = {
        .init         = panel_init,
        .full_refresh = panel_full_refresh,
        .write_full   = panel_write_full,
        .partial      = panel_partial,
        .power_off    = panel_power_off,
        .deep_sleep   = panel_deep_sleep,
        .probe        = NULL,      /* probe 环境启用时补（§14.3） */
        .write_planes = NULL,      /* Phase 6 多平面色彩面板用（§9.3） */
        .diag         = bus_diag_uc, /* T1.8：UC 族 FLG 0x71 双读（GxEPD2 路径仅诊断用 bus 位掩读，刷新仍走 s_epd2） */
    },
};
