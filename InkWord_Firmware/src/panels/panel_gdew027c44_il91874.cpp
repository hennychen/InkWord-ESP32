/**
 * @file panel_gdew027c44_il91874.cpp
 * @brief 2.7" 176x264 BWR 三色面板单元（L0）—— IL91874 手写序列
 *        （PANEL_COMPAT_DESIGN §十六 SOP 第四面板 / 首个 IL 系色彩面板）
 *
 * 面板：2.7" 264x176 黑白红三色，24P FPC（EVK011 J1 座原生规格）。
 * !! 型号勘误（2026-08-22 真机 bring-up）：原采购判型 WF0270T1PCZ2200E4
 * （22Pin/SSD1680）有误——实物 24Pin。判族证据链：上电 BUSY 轨迹规律
 * 脉冲 + RST 后回 idle=HIGH（SSD16xx 为 idle=LOW）→ IL/UC 系 LOW=忙；
 * GxEPD2_270c（GDEW027C44）序列一比一点亮，四象限+全白交替显示正确，
 * 全刷 0x12 后 BUSY 14720ms 稳定复现。型号按序列兼容性推定 GDEW027C44
 * 同族（Waveshare 2.7inch e-Paper B V1 / EK79652 兼容）；真 22Pin
 * WF0270 到货后走 panel_wf0270_ssd1680.cpp 另行验证。
 *
 * 序列来源（权威，一比一移植）：GxEPD2 1.6.9 GxEPD2_270c.cpp
 * （_InitDisplay 初始代码 / _Init_Full 五组 LUT / _setPartialRamArea_270c
 * 窗口 / _Update_Full / _PowerOff / hibernate），先经 Info/ink_test
 * bring-up 工具真机验证后落此（勿直接抄库源码绕过验证口径）。
 *
 * !! RAM 写入铁律（2026-08-22 真机勘误，区别于 SSD 系）：EK79652 要求数据
 * 逐字节独立 CS 事务锁存——单事务 transfer 连发（SSD1619/E042A13 实证
 * 可用）在本屏命令/窗口全生效但 RAM 数据不落位：0x12 后 14.7s 真实波形
 * 而屏显黑红颗粒（上电随机态）。GxEPD2_270c 所有写入均每字节独立 CS
 * 周期（_writeDataPGM_sCS 的 sCS = separate CS）佐证。本单元
 * epd_write_plane 逐字节独立事务，勿改回连发。
 *
 * 双 RAM 语义（IL91874，B/W 位与 SSD16xx 相反）：0x14 B/W RAM bit=1
 * 黑 / 0x15 color RAM bit=1 红（GxEPD2 writeImage _writeData(~data)
 * 反证）。driver 层 plane 约定 bit=1 白（epd_driver.cpp canvas_to_panel
 * 注释），故本单元发 0x14 前按字节取反；0x15 bit=1=红 语义一致直通。
 *
 * 时序（2026-08-22 真机实测）：官方 LUT 全刷 0x12 后 BUSY LOW 14720ms；
 * RST 脉冲 20ms；busy_level=0（LOW=忙，UC/IL 系）。
 * 快刷（2026-08-22 LUT 提速实验定档 E4）：五组 LUT 重复数 byte5 等比例
 * 25% 压缩（1157→341 帧），BUSY 4359ms，四象限/全白目验无残影；刷新
 * 时长 = 帧数 x 12.72ms 线性模型（datasheet §8.2.16 格式破译 + ink_test
 * E0~E4 五档真机实测拟合误差 <0.5%）。默认每 8 次快刷插 1 次官方深刷
 * 抗残影累积（Kindle 式全刷周期，G027_FAST_PER_DEEP 可调）。
 *
 * 无状态设计（desc 头注释铁律）：do_refresh 前置 s_ready 检查，
 * power_off/deep_sleep 后归零，下次刷新自动 RST 唤醒 + 完整重配
 * （GxEPD2 _InitDisplay 每次 _reset + 全量重发同风格）。
 *
 * BW 黑白快刷实验（2026-09-04）：新增 E5 档 LUT（byte5 再压缩 ~50%，
 * ~170 帧 / ~2.2s）+ BW-only 三组 LUT（跳过 RED/WHITE 省传输）+
 * panel_partial_bw() 入口（仅写 0x14 跳过 0x15）。每 4 次 BW 快刷插
 * 1 次官方深刷清残影。!! 需真机实测验证：IL91874 三色控制器即使
 * 不发红数据，内部波形引擎可能仍执行完整 5 段序列。
 */
#include "../epd_panel.h"
#include "../gpio_config.h"
#include "epd_bus.h"    /* T1.1：SPI 原语/等待收敛层 */

#include <Arduino.h>
#include <SPI.h>

/* 前置声明：ops 实现引用 desc 几何/时序字段 */
extern const epd_panel_desc_t g_panel_gdew027c44;

static bool s_ready = false;   /* init 完成（power_off/deep_sleep 归零） */


/* RAM 平面写入：逐字节独立 CS 事务（!! 勿改单事务连发，
 * EK79652 锁存要求，文件头铁律；invert=true 发送前按字节取反，
 * 用于 0x14 的 driver 白位→RAM 黑位适配）—— T1.1 后由
 * epd_bus bus_dat 实现（单字节独立事务语义不变） */
static void epd_write_plane(const uint8_t *p, size_t n, bool invert)
{
    for (size_t i = 0; i < n; i++)
        bus_dat(invert ? (uint8_t)~p[i] : p[i]);
}

/* —— 五组 LUT（IL91874 无 OTP 三色波形，PSR 0xaf 选 register LUT 后
 * 必须显式下发）。两组：
 *   LUT_*（原名）= GxEPD2_270c lut_20~24 原值，GDEW027C44 官方波形
 *     （1157 帧 / 14.7s）——深刷与上电默认；
 *   LUT_*_FAST = E4 快刷表：仅重复数 byte5 压缩（S2=2 S3=4 S4=2
 *     S5=1 S6=2，341 帧 / 4.4s），其余字节与官方逐字节一致
 *     （2026-08-22 ink_test 五档真机实验+目验定档，见文件头）。 —— */
static const uint8_t LUT_VCOM[] = {          /* 0x20 vcom */
    0x00, 0x00, 0x00, 0x1A, 0x1A, 0x00, 0x00, 0x01,
    0x00, 0x0A, 0x0A, 0x00, 0x00, 0x08, 0x00, 0x0E, 0x01, 0x0E, 0x01, 0x10,
    0x00, 0x0A, 0x0A, 0x00, 0x00, 0x08, 0x00, 0x04, 0x10, 0x00, 0x00, 0x05,
    0x00, 0x03, 0x0E, 0x00, 0x00, 0x0A, 0x00, 0x23, 0x00, 0x00, 0x00, 0x01,
};
static const uint8_t LUT_WW[] = {            /* 0x21 ww */
    0x90, 0x1A, 0x1A, 0x00, 0x00, 0x01, 0x40, 0x0A, 0x0A, 0x00, 0x00, 0x08,
    0x84, 0x0E, 0x01, 0x0E, 0x01, 0x10, 0x80, 0x0A, 0x0A, 0x00, 0x00, 0x08,
    0x00, 0x04, 0x10, 0x00, 0x00, 0x05, 0x00, 0x03, 0x0E, 0x00, 0x00, 0x0A,
    0x00, 0x23, 0x00, 0x00, 0x00, 0x01,
};
static const uint8_t LUT_RED[] = {           /* 0x22 bw r */
    0xA0, 0x1A, 0x1A, 0x00, 0x00, 0x01, 0x00, 0x0A, 0x0A, 0x00, 0x00, 0x08,
    0x84, 0x0E, 0x01, 0x0E, 0x01, 0x10, 0x90, 0x0A, 0x0A, 0x00, 0x00, 0x08,
    0xB0, 0x04, 0x10, 0x00, 0x00, 0x05, 0xB0, 0x03, 0x0E, 0x00, 0x00, 0x0A,
    0xC0, 0x23, 0x00, 0x00, 0x00, 0x01,
};
static const uint8_t LUT_WHITE[] = {         /* 0x23 wb w */
    0x90, 0x1A, 0x1A, 0x00, 0x00, 0x01, 0x40, 0x0A, 0x0A, 0x00, 0x00, 0x08,
    0x84, 0x0E, 0x01, 0x0E, 0x01, 0x10, 0x80, 0x0A, 0x0A, 0x00, 0x00, 0x08,
    0x00, 0x04, 0x10, 0x00, 0x00, 0x05, 0x00, 0x03, 0x0E, 0x00, 0x00, 0x0A,
    0x00, 0x23, 0x00, 0x00, 0x00, 0x01,
};
static const uint8_t LUT_BLACK[] = {         /* 0x24 bb b */
    0x90, 0x1A, 0x1A, 0x00, 0x00, 0x01, 0x20, 0x0A, 0x0A, 0x00, 0x00, 0x08,
    0x84, 0x0E, 0x01, 0x0E, 0x01, 0x10, 0x10, 0x0A, 0x0A, 0x00, 0x00, 0x08,
    0x00, 0x04, 0x10, 0x00, 0x00, 0x05, 0x00, 0x03, 0x0E, 0x00, 0x00, 0x0A,
    0x00, 0x23, 0x00, 0x00, 0x00, 0x01,
};
/* E4 快刷表（byte5: S2 08→02 S3 10→04 S4 08→02 S5 05→01 S6 0A→02） */
static const uint8_t LUT_VCOM_FAST[] = {
    0x00, 0x00, 0x00, 0x1A, 0x1A, 0x00, 0x00, 0x01,
    0x00, 0x0A, 0x0A, 0x00, 0x00, 0x02, 0x00, 0x0E, 0x01, 0x0E, 0x01, 0x04,
    0x00, 0x0A, 0x0A, 0x00, 0x00, 0x02, 0x00, 0x04, 0x10, 0x00, 0x00, 0x01,
    0x00, 0x03, 0x0E, 0x00, 0x00, 0x02, 0x00, 0x23, 0x00, 0x00, 0x00, 0x01,
};
static const uint8_t LUT_WW_FAST[] = {
    0x90, 0x1A, 0x1A, 0x00, 0x00, 0x01, 0x40, 0x0A, 0x0A, 0x00, 0x00, 0x02,
    0x84, 0x0E, 0x01, 0x0E, 0x01, 0x04, 0x80, 0x0A, 0x0A, 0x00, 0x00, 0x02,
    0x00, 0x04, 0x10, 0x00, 0x00, 0x01, 0x00, 0x03, 0x0E, 0x00, 0x00, 0x02,
    0x00, 0x23, 0x00, 0x00, 0x00, 0x01,
};
static const uint8_t LUT_RED_FAST[] = {
    0xA0, 0x1A, 0x1A, 0x00, 0x00, 0x01, 0x00, 0x0A, 0x0A, 0x00, 0x00, 0x02,
    0x84, 0x0E, 0x01, 0x0E, 0x01, 0x04, 0x90, 0x0A, 0x0A, 0x00, 0x00, 0x02,
    0xB0, 0x04, 0x10, 0x00, 0x00, 0x01, 0xB0, 0x03, 0x0E, 0x00, 0x00, 0x02,
    0xC0, 0x23, 0x00, 0x00, 0x00, 0x01,
};
static const uint8_t LUT_WHITE_FAST[] = {
    0x90, 0x1A, 0x1A, 0x00, 0x00, 0x01, 0x40, 0x0A, 0x0A, 0x00, 0x00, 0x02,
    0x84, 0x0E, 0x01, 0x0E, 0x01, 0x04, 0x80, 0x0A, 0x0A, 0x00, 0x00, 0x02,
    0x00, 0x04, 0x10, 0x00, 0x00, 0x01, 0x00, 0x03, 0x0E, 0x00, 0x00, 0x02,
    0x00, 0x23, 0x00, 0x00, 0x00, 0x01,
};
static const uint8_t LUT_BLACK_FAST[] = {
    0x90, 0x1A, 0x1A, 0x00, 0x00, 0x01, 0x20, 0x0A, 0x0A, 0x00, 0x00, 0x02,
    0x84, 0x0E, 0x01, 0x0E, 0x01, 0x04, 0x10, 0x0A, 0x0A, 0x00, 0x00, 0x02,
    0x00, 0x04, 0x10, 0x00, 0x00, 0x01, 0x00, 0x03, 0x0E, 0x00, 0x00, 0x02,
    0x00, 0x23, 0x00, 0x00, 0x00, 0x01,
};
/* E5 黑白快刷表（byte5 再压缩 ~50%：S2=1 S3=2 S4=1 S5=1 S6=1，
 * ~170 帧 / ~2.2s；仅 BW 三组 LUT，跳过 RED/WHITE 省传输时间） */
static const uint8_t LUT_VCOM_BW[] = {
    0x00, 0x00, 0x00, 0x1A, 0x1A, 0x00, 0x00, 0x01,
    0x00, 0x0A, 0x0A, 0x00, 0x00, 0x01, 0x00, 0x0E, 0x01, 0x0E, 0x01, 0x02,
    0x00, 0x0A, 0x0A, 0x00, 0x00, 0x01, 0x00, 0x04, 0x10, 0x00, 0x00, 0x01,
    0x00, 0x03, 0x0E, 0x00, 0x00, 0x01, 0x00, 0x23, 0x00, 0x00, 0x00, 0x01,
};
static const uint8_t LUT_WW_BW[] = {
    0x90, 0x1A, 0x1A, 0x00, 0x00, 0x01, 0x40, 0x0A, 0x0A, 0x00, 0x00, 0x01,
    0x84, 0x0E, 0x01, 0x0E, 0x01, 0x02, 0x80, 0x0A, 0x0A, 0x00, 0x00, 0x01,
    0x00, 0x04, 0x10, 0x00, 0x00, 0x01, 0x00, 0x03, 0x0E, 0x00, 0x00, 0x01,
    0x00, 0x23, 0x00, 0x00, 0x00, 0x01,
};
static const uint8_t LUT_BLACK_BW[] = {
    0x90, 0x1A, 0x1A, 0x00, 0x00, 0x01, 0x20, 0x0A, 0x0A, 0x00, 0x00, 0x01,
    0x84, 0x0E, 0x01, 0x0E, 0x01, 0x02, 0x10, 0x0A, 0x0A, 0x00, 0x00, 0x01,
    0x00, 0x04, 0x10, 0x00, 0x00, 0x01, 0x00, 0x03, 0x0E, 0x00, 0x00, 0x01,
    0x00, 0x23, 0x00, 0x00, 0x00, 0x01,
};

static void il_write_lut(uint8_t cmd, const uint8_t *lut, size_t n)
{
    bus_cmd(cmd);
    epd_write_plane(lut, n, false);   /* LUT 亦逐字节独立 CS（GxEPD2 sCS 同款） */
}

/* RAM 全屏窗口（_setPartialRamArea_270c 原格式 8 字节：x>>8, x&0xf8,
 * y>>8, y&0xff, w>>8, w&0xf8, h>>8, h&0xff；全屏 x=0 y=0 w=176 h=264） */
static void set_full_window(uint8_t cmd)
{
    bus_cmd(cmd);
    bus_dat(0x00); bus_dat(0x00);                  /* x = 0 */
    bus_dat(0x00); bus_dat(0x00);                  /* y = 0 */
    bus_dat(0x00); bus_dat((uint8_t)(g_panel_gdew027c44.panel_w & 0xF8));
                                                  /* w = 176 (0xB0) */
    bus_dat((uint8_t)(g_panel_gdew027c44.panel_h >> 8));
    bus_dat((uint8_t)(g_panel_gdew027c44.panel_h & 0xFF)); /* h = 264 (0x108) */
}

static int panel_init_impl(bool fast)
{
    /* GxEPD2_270c _InitDisplay + _Init_Full + _PowerOn 一比一
     * （Info/ink_test 真机验证口径；fast=true 选 E4 快刷 LUT） */
    pinMode(EPD_RESET_PIN, OUTPUT);
    pinMode(EPD_CS_PIN, OUTPUT);
    pinMode(EPD_DC_PIN, OUTPUT);
    pinMode(EPD_BUSY_PIN, INPUT);
    digitalWrite(EPD_CS_PIN, HIGH);
    digitalWrite(EPD_DC_PIN, LOW);

    digitalWrite(EPD_RESET_PIN, HIGH);
    delay(10);
    digitalWrite(EPD_RESET_PIN, LOW);
    delay(g_panel_gdew027c44.rst_pulse_ms);   /* GxEPD2 默认 20ms 脉冲 */
    digitalWrite(EPD_RESET_PIN, HIGH);
    delay(10);
    bus_wait_idle(&g_panel_gdew027c44, 5000);                    /* 复位后 boot 自检（忙→闲） */

    bus_cmd(0x01);                            /* 驱动配置（gate 软起等） */
    bus_dat(0x03); bus_dat(0x00); bus_dat(0x2b); bus_dat(0x2b); bus_dat(0x09);
    bus_cmd(0x06);                            /* boost 软启动 */
    bus_dat(0x07); bus_dat(0x07); bus_dat(0x17);
    bus_cmd(0xF8); bus_dat(0x60); bus_dat(0xA5);   /* 命令解锁序列 ×5 */
    bus_cmd(0xF8); bus_dat(0x89); bus_dat(0xA5);
    bus_cmd(0xF8); bus_dat(0x90); bus_dat(0x00);
    bus_cmd(0xF8); bus_dat(0x93); bus_dat(0x2A);
    bus_cmd(0xF8); bus_dat(0x73); bus_dat(0x41);
    bus_cmd(0x16); bus_dat(0x00);             /* 增强命令复位 */
    bus_cmd(0x00); bus_dat(0xaf);             /* PSR: by register LUT */
    bus_cmd(0x30); bus_dat(0x3a);             /* PLL 90Hz */
    bus_cmd(0x61);                            /* 分辨率 176x264（竖屏原生） */
    bus_dat(0x00); bus_dat((uint8_t)g_panel_gdew027c44.panel_w);
    bus_dat((uint8_t)(g_panel_gdew027c44.panel_h >> 8));
    bus_dat((uint8_t)(g_panel_gdew027c44.panel_h & 0xFF));
    bus_cmd(0x82); bus_dat(0x12);             /* VCOM_DC */
    bus_cmd(0x50); bus_dat(0x87);             /* CDI VCOM 与边框 */

    if (fast) {   /* E4 快刷表（LUT 区注释：仅 byte5 重复数压缩） */
        il_write_lut(0x20, LUT_VCOM_FAST,   sizeof(LUT_VCOM_FAST));
        il_write_lut(0x21, LUT_WW_FAST,     sizeof(LUT_WW_FAST));
        il_write_lut(0x22, LUT_RED_FAST,    sizeof(LUT_RED_FAST));
        il_write_lut(0x23, LUT_WHITE_FAST,  sizeof(LUT_WHITE_FAST));
        il_write_lut(0x24, LUT_BLACK_FAST,  sizeof(LUT_BLACK_FAST));
    } else {      /* 官方深刷表（GxEPD2_270c 原值） */
        il_write_lut(0x20, LUT_VCOM,   sizeof(LUT_VCOM));
        il_write_lut(0x21, LUT_WW,     sizeof(LUT_WW));
        il_write_lut(0x22, LUT_RED,    sizeof(LUT_RED));
        il_write_lut(0x23, LUT_WHITE,  sizeof(LUT_WHITE));
        il_write_lut(0x24, LUT_BLACK,  sizeof(LUT_BLACK));
    }

    bus_cmd(0x04);                            /* power on */
    bus_wait_idle(&g_panel_gdew027c44, 5000);

    s_ready = true;
    return 0;
}

/* —— BW-only 初始化（黑白快刷实验，2026-09-04）——
 * 仅下发 VCOM + WW + BB 三组 LUT（跳过 RED 0x22 / WHITE 0x23），
 * 理论省 ~40% LUT 传输时间 + 控制器跳过红粒子驱动段。
 * !! 注意：IL91874 是三色控制器，即使不发红数据，内部波形引擎
 * 可能仍执行完整 5 段序列——需真机实测验证时间是否缩短。 */
static int panel_init_bw(void)
{
    /* RST + 初始序列同 panel_init_impl，仅 LUT 不同 */
    pinMode(EPD_RESET_PIN, OUTPUT);
    pinMode(EPD_CS_PIN, OUTPUT);
    pinMode(EPD_DC_PIN, OUTPUT);
    pinMode(EPD_BUSY_PIN, INPUT);
    digitalWrite(EPD_CS_PIN, HIGH);
    digitalWrite(EPD_DC_PIN, LOW);

    digitalWrite(EPD_RESET_PIN, HIGH);
    delay(10);
    digitalWrite(EPD_RESET_PIN, LOW);
    delay(g_panel_gdew027c44.rst_pulse_ms);
    digitalWrite(EPD_RESET_PIN, HIGH);
    delay(10);
    bus_wait_idle(&g_panel_gdew027c44, 5000);

    bus_cmd(0x01);
    bus_dat(0x03); bus_dat(0x00); bus_dat(0x2b); bus_dat(0x2b); bus_dat(0x09);
    bus_cmd(0x06);
    bus_dat(0x07); bus_dat(0x07); bus_dat(0x17);
    bus_cmd(0xF8); bus_dat(0x60); bus_dat(0xA5);
    bus_cmd(0xF8); bus_dat(0x89); bus_dat(0xA5);
    bus_cmd(0xF8); bus_dat(0x90); bus_dat(0x00);
    bus_cmd(0xF8); bus_dat(0x93); bus_dat(0x2A);
    bus_cmd(0xF8); bus_dat(0x73); bus_dat(0x41);
    bus_cmd(0x16); bus_dat(0x00);
    bus_cmd(0x00); bus_dat(0xaf);             /* PSR: by register LUT */
    bus_cmd(0x30); bus_dat(0x3a);             /* PLL 90Hz */
    bus_cmd(0x61);
    bus_dat(0x00); bus_dat((uint8_t)g_panel_gdew027c44.panel_w);
    bus_dat((uint8_t)(g_panel_gdew027c44.panel_h >> 8));
    bus_dat((uint8_t)(g_panel_gdew027c44.panel_h & 0xFF));
    bus_cmd(0x82); bus_dat(0x12);             /* VCOM_DC */
    bus_cmd(0x50); bus_dat(0x87);             /* CDI */

    /* BW-only 三组 LUT（跳过 0x22 RED + 0x23 WHITE） */
    il_write_lut(0x20, LUT_VCOM_BW,  sizeof(LUT_VCOM_BW));
    il_write_lut(0x21, LUT_WW_BW,    sizeof(LUT_WW_BW));
    il_write_lut(0x24, LUT_BLACK_BW, sizeof(LUT_BLACK_BW));

    bus_cmd(0x04);                            /* power on */
    bus_wait_idle(&g_panel_gdew027c44, 5000);

    s_ready = true;
    return 0;
}

/* ops.init：上电默认官方深刷表（保守；desc.full_ms=快刷口径仅为日志
 * 提示，上电首刷走深刷无妨） */
static int panel_init(void)
{
    return panel_init_impl(false);
}

/* 双平面写入 + 真全刷（GxEPD2 writeImage + _Update_Full 一比一）：
 * planes[0] = B/W 平面（driver 语义 bit=1 白）→ 取反发 0x14（RAM 1=黑）；
 * planes[1] = 红位平面（bit=1 红）→ 直通发 0x15（RAM 1=红）；
 * 刷新 0x12 直接激活（UC/IL 系标准，非 SSD16xx 的 0x22/0x20 序列） */
/* 快/深刷调度：每 G027_FAST_PER_DEEP 次快刷插 1 次官方深刷，抗残影
 * 累积（E4 单次目验无残影，长期累积未验证，Kindle 式全刷周期保守
 * 设计；宏改 0 = 恒快刷，调用点 deep 写死 true = 恒深刷回退） */
#define G027_FAST_PER_DEEP  8
static uint32_t s_refresh_seq = 0;

static bool refresh_pick_deep(void)
{
    const bool deep = (G027_FAST_PER_DEEP > 0) &&
        (s_refresh_seq % (G027_FAST_PER_DEEP + 1) == G027_FAST_PER_DEEP);
    s_refresh_seq++;
    return deep;
}

static int do_refresh(const uint8_t *bw_plane, const uint8_t *red_plane)
{
    /* 换 LUT 须 RST 重配整套下发（无状态铁律；重配 ~150ms 相对波形
     * 4.4s/14.7s 可忽略），每刷重 init（GxEPD2 每次 _reset 同风格） */
    const bool deep = refresh_pick_deep();
    Serial.printf("[G027] refresh #%u: %s LUT\n",
                  (unsigned)(s_refresh_seq - 1),
                  deep ? "DEEP (official 14.7s)" : "FAST (E4 4.4s)");
    if (panel_init_impl(!deep) != 0) return -1;
    const size_t plane_bytes =
        (size_t)(g_panel_gdew027c44.panel_w / 8) * g_panel_gdew027c44.panel_h;

    set_full_window(0x14);                    /* black RAM 全屏窗口 */
    epd_write_plane(bw_plane, plane_bytes, true);
    set_full_window(0x15);                    /* red RAM 全屏窗口 */
    epd_write_plane(red_plane, plane_bytes, false);

    bus_cmd(0x12);                            /* display refresh */
    bus_wait_busy(&g_panel_gdew027c44, g_panel_gdew027c44.busy_timeout_ms);                      /* 三色波形 ~14.7s：等完成再下电 */
    return 0;
}

static int panel_full_refresh(const uint8_t *frame)
{
    /* frame：双平面连续布局（Phase 2 约定）：plane_count x panel_w/8
     * x panel_h 字节，[0]=B/W 白位平面、[1]=红位平面（driver 语义） */
    const size_t plane_bytes =
        (size_t)(g_panel_gdew027c44.panel_w / 8) * g_panel_gdew027c44.panel_h;
    return do_refresh(frame, frame + plane_bytes);
}

static int panel_write_full(const uint8_t *frame)
{
    /* 竖屏原始帧直通全刷（epd_full_refresh / LAN 语义）：frame=NULL
     * 清白（RAM 语义：0x14 全 0x00 = 白、0x15 全 0x00 = 无红）。
     * 注：本路径不维护 prev 帧一致性，调用方须强制下一次全刷
     * （ui_force_full_refresh_next 已保证） */
    if (!frame) {
        const bool deep = refresh_pick_deep();  /* 清白同享快/深调度 */
        if (panel_init_impl(!deep) != 0) return -1;
        const size_t plane_bytes =
            (size_t)(g_panel_gdew027c44.panel_w / 8) * g_panel_gdew027c44.panel_h;
        set_full_window(0x14);
        for (size_t i = 0; i < plane_bytes; i++) bus_dat(0x00);
        set_full_window(0x15);
        for (size_t i = 0; i < plane_bytes; i++) bus_dat(0x00);
        bus_cmd(0x12);
        bus_wait_busy(&g_panel_gdew027c44, g_panel_gdew027c44.busy_timeout_ms);
        return 0;
    }
    return panel_full_refresh(frame);
}

/* —— BW-only 黑白快刷（2026-09-04 实验）——
 * 仅写 0x14 B/W 平面（跳过 0x15 红平面），用 BW 三组 LUT。
 * 预期：LUT 传输省 ~40% + 波形段省红粒子驱动 → ~2s 级刷新。
 * !! 残影风险高：每 G027_BW_FAST_PER_DEEP 次 BW 快刷须插 1 次
 * 官方深刷清残影（保守 4 次，E5 压缩激进）。 */
#define G027_BW_FAST_PER_DEEP  4
static uint32_t s_bw_refresh_seq = 0;

static int panel_partial_bw(const uint8_t *prev, const uint8_t *new_,
                            uint8_t passes)
{
    (void)passes;  /* BW 快刷固定 1 pass（E5 波形已含足够帧数） */
    const bool deep = (G027_BW_FAST_PER_DEEP > 0) &&
        (s_bw_refresh_seq % (G027_BW_FAST_PER_DEEP + 1) == G027_BW_FAST_PER_DEEP);
    s_bw_refresh_seq++;

    Serial.printf("[G027] BW refresh #%u: %s\n",
                  (unsigned)(s_bw_refresh_seq - 1),
                  deep ? "DEEP (official 14.7s)" : "BW-FAST (E5 ~2.2s)");

    if (deep) {
        /* 深刷回退：走完整三色序列清残影 */
        if (panel_init_impl(false) != 0) return -1;
    } else {
        /* BW 快刷：仅 BW 三组 LUT */
        if (panel_init_bw() != 0) return -1;
    }

    const size_t plane_bytes =
        (size_t)(g_panel_gdew027c44.panel_w / 8) * g_panel_gdew027c44.panel_h;

    /* 写 B/W 平面（取反：driver bit=1 白 → RAM bit=1 黑） */
    set_full_window(0x14);
    epd_write_plane(new_, plane_bytes, true);
    /* !! 跳过 0x15 红平面写入（BW 模式无红数据） */

    bus_cmd(0x12);                            /* display refresh */
    bus_wait_busy(&g_panel_gdew027c44, g_panel_gdew027c44.busy_timeout_ms);
    return 0;
}

static int panel_write_planes(const uint8_t *const *planes)
{
    /* 多平面统一入口（§9.3）：planes[] 指针数组（plane_count 项） */
    return do_refresh(planes[0], planes[1]);
}

static void panel_power_off(void)
{
    /* GxEPD2_270c _PowerOff 一比一：0x02 关高压。归零 s_ready —— 下次
     * 刷新完整重配（无状态铁律，不赌关电后 LUT/窗口存活） */
    if (!s_ready) return;
    bus_cmd(0x02);
    bus_wait_idle(&g_panel_gdew027c44, g_panel_gdew027c44.busy_timeout_ms);
    s_ready = false;
}

static void panel_deep_sleep(void)
{
    /* GxEPD2_270c hibernate 一比一：0x07 check 0xA5 深睡（~µA 级），
     * RST 硬复位唤醒 + panel_init 重初始化 */
    bus_cmd(0x07); bus_dat(0xA5);
    s_ready = false;
}

/* —— desc 注册（第四面板单元，首个 IL 系色彩面板）——
 * 时序：真机实测口径（2026-08-22 bring-up：全刷 14720ms 稳定复现）；
 * BUSY 极性 LOW=忙（上电轨迹 + RST 应答 + 0x12 波形三重实证）；
 * 几何：COG 竖屏 176x264 + gfx_rotation=1 → UI 横屏 264x176
 * （LAYOUT_SMALL 档，与 wf0270 几何同构） */
const epd_panel_desc_t g_panel_gdew027c44 = {
    .name       = "gdew027c44_il91874",
    .controller = EPD_CTRL_IL91874,
    .otp_signature = 0,           /* IL91874 无 0x2F 寄存器，不参与自动识别 */
    .panel_w    = 176,
    .panel_h    = 264,
    .gfx_rotation = 1,       /* 物理竖屏 176x264 → UI 横屏 264x176 */
    .dpi         = 118,       /* 对角 PPI（诊断字段：2.7" 对角 317px；换算 mm=px÷PPI×25.4） */
    .color_mode = EPD_COLOR_3C,
    .plane_count = 2,        /* B/W 白位平面 + 红位平面（§9.3） */
    .palette    = {
        /* 逻辑色 → 平面置位掩码（driver 层语义，RAM 极性差异封装于
         * 面板单元 do_refresh 取反，同 E042A13 注释口径）：
         * bit0=B/W 平面白位，bit1=红平面红位 */
        [EPD_GFX_WHITE]  = 0x01,   /* (bw=1,red=0) → 白 */
        [EPD_GFX_BLACK]  = 0x00,   /* (0,0) → 黑 */
        [EPD_GFX_ACCENT] = 0x02,   /* (0,1) → 红（真强调色可用） */
        [EPD_GFX_AUX]    = 0x00,   /* 3C 无第四色 → 退化黑（§9.2） */
    },
    .accent_rgb = 0xFF0000,    /* 红（LAN 量化调色板注入） */
    .fb_location = EPD_FB_AUTO,   /* 双帧+画布 ~35KB < 128KB → 全 SRAM
                                   * （§10.2；与 WiFi/BLE 共存余量足） */
    .rst_pulse_ms = 20,           /* GxEPD2 默认复位脉宽（真机验证） */
    .busy_level = 0,              /* BUSY=LOW 忙（UC/IL 系，与
                                   * SSD16xx HIGH=忙相反） */
    .busy_timeout_ms = 20000,     /* 深刷忙 14720ms + 余量（快刷 4359ms
                                   * 远在内，单字段覆盖两档） */
    .power_on_ms  = 50,           /* 0x04 后自检，保守近似值 */
    .power_off_ms = 50,
    .full_ms    = 5000,           /* E4 快刷实测 4359ms（2026-08-22 LUT
                                   * 提速定档）+余量；每 9 次刷新含 1 次
                                   * 官方深刷 14720ms（见文件头/LUT 注释） */
    .partial_ms = 2500,           /* BW 快刷 E5 预计 ~2.2s + 余量
                                   * （深刷回退 14.7s 由 busy_timeout 覆盖） */
    .partial_enabled = false,     /* !! 临时回退（2026-09-04）：BW 快刷实测
                                   * 显示不清晰，待 LUT 优化后重新启用 */
    .passes     = 1,
    .partial_count_full_refresh = 4, /* BW 快刷每 4 次插 1 次深刷清残影
                                      * （E5 压缩激进，保守周期） */
    .window_8align = true,        /* 窗口 x/w 8 像素对齐（全屏 176 ✓） */
    .ops = {
        .init         = panel_init,
        .full_refresh = panel_full_refresh,
        .write_full   = panel_write_full,
        .partial      = panel_partial_bw,  /* BW 黑白快刷实验入口
                                            * （partial_enabled=true 时 L3 触达） */
        .power_off    = panel_power_off,
        .deep_sleep   = panel_deep_sleep,
        .probe        = NULL,     /* IL91874 无 FLG/版本读寄存器
                                  * （0x71 位掩读恒 0x00 而屏正常，
                                  * 2026-08-22 真机实证），无 probe 路径 */
        .write_planes = panel_write_planes,
        .diag         = NULL, /* T1.8：IL91874 无 FLG/版本寄存器 */
    },
};
