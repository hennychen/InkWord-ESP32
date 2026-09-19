/**
 * @file panel_gdeq031t10_uc8253.cpp
 * @brief 3.1" 面板单元（L0）—— GxEPD2_gdeq031t10 包装 + desc 注册
 *
 * 面板：GDEQ031T10 3.1" 240x320 BW，UC8253 COG，24P FPC 0.5mm
 * 骨架取自 panel_depg0370_uc8253.cpp（同 UC8253 控制器族）。
 *
 * 屏体身份（2026-09-19 实录）：本条目挂在**当前上机的实物屏**上，
 * 其排线丝印 `P310011-MF1-A`（24P / 0.5mm）。卖家规格书
 * （Info/3.1黑白IC UC8253.md）明确「和佳显的排线不同」，且 Info/ 内
 * 只有 GDEQ031T10 的取模说明与 demo 包、**没有 24P 引脚定义表**
 * （UC8253.pdf 的 PIN DESCRIPTION 是 COG bump 表，非模块 FPC 次序），
 * 故条目名 GDEQ031T10 只是 demo 包命名沿袭，未经屏体规格书证实。
 * 真机侧已知事实：5V 供电下 3.1" 与 3.7" 两块屏均观察到刷新动作
 * （升压 VGH/VGL 与 VCI/GND 脚位没错位，"FPC 镜像反插"假设排除）；
 * 与丝印同族的 P426010-MF1-A 是本项目 GDEQ0426T82 的排线，可作为
 * 同厂 FPC 编号体系旁证。缺的是屏体 datasheet，拿到后补进 desc。
 *
 * 规格：
 *   分辨率 240x320（竖屏原生），SPI，黑白
 *   全刷 3s / 快刷 1s / 局刷 0.5s
 *   视域 62.72x47.04mm，对角 ≈129 PPI
 *   规格建议：快刷/局刷连续 5 次后加一次全屏刷新减少残影
 *
 * 真机实测（2026-09-19，探针 env:gdeq031t10-timing，屏上纯单色帧只取
 * 时长，室温）：全刷 0x12 净忙窗 3088ms（整条路径墙钟 3130ms）、快刷
 * 1060ms、局刷单遍 652ms／量产双遍 1304ms。
 *
 * 归因结论（同一探针 G 臂单变量对照）：本项目全刷比库内官方同类慢
 * 3 倍，全部差值来自官方 useFastFullUpdate 的 CCSET E0=0x02 +
 * TSSET E5=0x5A（强制内部温度）—— 官方真序复刻不带 E0/E5 是
 * 3088ms，带上即 1018ms。即 initFullDemo 的 3s 是屏体自动温补波形
 * 本身的长度（与规格标称一致），不是驱动 bug；本 initFastDemo 路径
 * 实测 1060ms 即官方口径，需要 1s 全刷时可用，代价是官方自述的低温
 * 欠驱动风险。详见 ../GxEPD2_gdeq031t10.cpp 文件头归因表。
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
 * 时序取 GxEPD2_gdeq031t10.h 静态属性 + 规格书标称值 */
const epd_panel_desc_t g_panel_gdeq031t10 = {
    .name       = "gdeq031t10_uc8253",
    .controller = EPD_CTRL_UC8253,
    .otp_signature = 0,           /* 待实测指纹（自动识别暂不启用） */
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
    .busy_timeout_ms = 5000,       /* 全刷实测忙窗 3088ms + 62% 余量：UC8253
                                    * 全刷走自动温补波形，低温下会拉长 */
    .power_on_ms  = 50,
    .power_off_ms = 50,
    .full_ms    = 3200,            /* 实测墙钟 3130ms（0x12 净忙窗 3088ms，
                                    * 2026-09-19 探针），规格标称 3s */
    .partial_ms = 1500,            /* 量产 passes=2 口径实测 1304ms 忙窗
                                    * （单遍 652ms，×2 线性叠加） */
    .partial_enabled = true,       /* 双 RAM 差分局刷 */
    .passes     = 2,               /* 默认双刷 */
    .partial_count_full_refresh = 8, /* 沿用 UC8253 族基线 8：规格书「5 次
                                      * 后加一次全刷」针对窗口局刷，本固件走
                                      * 无窗口整屏双 RAM 差分（2026-08-20 定
                                      * 稿，真机无残影），保养频率取族内一致
                                      * 值；真机 bring-up 按残影表现再校 */
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
