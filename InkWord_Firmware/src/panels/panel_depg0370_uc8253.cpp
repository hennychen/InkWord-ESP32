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

/* 前置声明：ops 实现引用 desc 几何字段（write_full 全屏参数） */
extern const epd_panel_desc_t g_panel_depg0370;

/* epd2 层驱动对象（epd2 直驱，不用 GxEPD2_BW 显示层）—— 全工程唯一样例
 * 化点，自 epd_driver.cpp 迁入（Phase 1，行为不变） */
static GxEPD2_374_DEPG0370 s_epd2(
    EPD_CS_PIN, EPD_DC_PIN, EPD_RESET_PIN, EPD_BUSY_PIN);

static int panel_init(void)
{
    /* 原 epd_driver_init 内 s_epd2.init(0, true, 20, false) 原样迁入：
     * 串口诊断关闭 / initial（复位+上电）/ 复位脉宽 20ms / 常规复位脚 */
    s_epd2.init(0, true, 20, false);
    return 0;
}

static int panel_full_refresh(const uint8_t *frame)
{
    /* demo 忠实版真全刷（Display_image_full_update）：硬复位（清局刷残留
     * E0/E5/PSR2）→ full 初始化（PSR+CDI=0x97）→ 无窗口整屏写 0x13
     * → 0x04/0x12/0x02。不能用 demoWriteDual 全屏参数代替 —— 窗口包裹的
     * 全屏刷驱动力不足，真机实测留残影（2026-08-18） */
    s_epd2.hwReset();
    s_epd2.initFullDemo();
    s_epd2.demoWriteFull(frame);
    s_epd2.updateDemoPartial(); /* update 序列全刷/局刷同款（0x04/0x12/0x02） */
    return 0;
}

static int panel_write_full(const uint8_t *frame)
{
    /* 竖屏原始帧直通全刷（epd_full_refresh / LAN 语义）：frame=NULL 清白。
     * 注：本路径不维护 prev 帧一致性，调用方须强制下一次全刷
     * （main.cpp ui_force_full_refresh_next() 已保证） */
    if (frame) {
        s_epd2.writeImageForFullRefresh(frame, 0, 0,
                                        g_panel_depg0370.panel_w,
                                        g_panel_depg0370.panel_h);
    } else {
        s_epd2.writeScreenBuffer(0xFF);
    }
    s_epd2.refresh(false); /* 全刷 */
    s_epd2.powerOff();
    return 0;
}

static int panel_partial(const uint8_t *prev, const uint8_t *new_, uint8_t passes)
{
    /* Plan B：无窗口整屏双 RAM 局刷（2026-08-20 取代窗口路径）：
     * 不发 0x91/0x90，整屏写 0x10 旧帧 + 0x13 新帧，COG 全屏差分驱动
     * 变化像素、跳过不变像素。窗口模式（demoWriteDual）三组参数实测均
     * 不能干净刷白（0x1f 留浅影 / 0x0d 无深睡不消失 / +深睡仍遮盖），
     * 与 GxEPD2「多数 UC 面板禁用 partial window」结论一致，弃用；
     * 代价：每次传整屏 12KB（SPI @20MHz ≈ 6ms，可忽略）。
     * passes 双刷：单次翻转不彻底时第二次 0x12 再驱动一遍；
     * 局刷自身无残影，全刷降为低频深度保养（standby 混合策略）。
     *
     * 单平面写（只写 0x13 省 ≈5ms）已实验证伪（2026-08-21）：0x12 后
     * COG 不自动 new→old，差分基准落后一帧 → 连续局刷残迹；
     * 0x10 必须每次显式重写 */
    s_epd2.hwReset();          /* 每次局刷前硬件复位，COG 状态归零 */
    s_epd2.initPartialDemo();
    s_epd2.demoWriteDualNoWindow(prev, new_);
    s_epd2.updateDemoPartial(passes);
    return 0;
}

static void panel_power_off(void)
{
    s_epd2.powerOff(); /* 0x02 关高压 rails（VCI 3.3V 保持供电） */
}

static void panel_deep_sleep(void)
{
    s_epd2.hibernate(); /* 0x02 下电 + 0x07/0xA5 深睡，可被硬件复位唤醒 */
}

/* —— desc 注册（首个面板单元，字段值实证见各注释）——
 * 时序取 GxEPD2_374_DEPG0370.h L34-37 静态属性（power_on 50 / power_off
 * 50 / full 1500 / partial 350ms）；阈值 8 为 main.cpp 局刷计数全刷阈值
 * 现值（待机 12 / 学习·阅读 8 分页差异化由调用方设置） */
const epd_panel_desc_t g_panel_depg0370 = {
    .name       = "depg0370_uc8253",
    .controller = EPD_CTRL_UC8253,
    .panel_w    = 240,
    .panel_h    = 416,
    .gfx_rotation = 1,       /* 横屏持机（gfx 416x240），Phase 2 转置泛化依据 */
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
