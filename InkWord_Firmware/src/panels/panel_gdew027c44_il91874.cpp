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
 * 时序（2026-08-22 真机实测）：全刷 0x12 后 BUSY LOW 14720ms；
 * RST 脉冲 20ms；busy_level=0（LOW=忙，UC/IL 系）。
 *
 * 无状态设计（desc 头注释铁律）：do_refresh 前置 s_ready 检查，
 * power_off/deep_sleep 后归零，下次刷新自动 RST 唤醒 + 完整重配
 * （GxEPD2 _InitDisplay 每次 _reset + 全量重发同风格）。
 */
#include "../epd_panel.h"
#include "../gpio_config.h"

#include <Arduino.h>
#include <SPI.h>

/* 前置声明：ops 实现引用 desc 几何/时序字段 */
extern const epd_panel_desc_t g_panel_gdew027c44;

static bool s_ready = false;   /* init 完成（power_off/deep_sleep 归零） */

/* —— SPI 底层（epd_driver_init 已 SPI.begin(7,-1,8,10)，此处事务直发） —— */
static void epd_cmd(uint8_t c)
{
    SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    digitalWrite(EPD_CS_PIN, LOW);
    digitalWrite(EPD_DC_PIN, LOW);    /* DC=0 命令 */
    SPI.transfer(c);
    digitalWrite(EPD_CS_PIN, HIGH);
    SPI.endTransaction();
}

/* 单字节数据：独立 CS 事务（EK79652 锁存要求，见文件头铁律） */
static void epd_dat(uint8_t d)
{
    SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    digitalWrite(EPD_CS_PIN, LOW);
    digitalWrite(EPD_DC_PIN, HIGH);   /* DC=1 数据 */
    SPI.transfer(d);
    digitalWrite(EPD_CS_PIN, HIGH);
    SPI.endTransaction();
}

/* RAM 平面写入：逐字节独立 CS 事务（!! 勿改单事务连发，见文件头铁律；
 * invert=true 发送前按字节取反，用于 0x14 的 driver 白位→RAM 黑位适配） */
static void epd_write_plane(const uint8_t *p, size_t n, bool invert)
{
    for (size_t i = 0; i < n; i++) epd_dat(invert ? (uint8_t)~p[i] : p[i]);
}

/* 等 BUSY 回空闲（IL91874 LOW=忙；init/关电路径用） */
static void panel_wait_idle(uint32_t timeout_ms)
{
    const uint32_t t0 = millis();
    while (digitalRead(EPD_BUSY_PIN) == g_panel_gdew027c44.busy_level &&
           millis() - t0 < timeout_ms)
        delay(10);
}

/* 0x12 刷新后 BUSY 两段式等待（诊断口径同 E042A13 bring-up）：
 *   ① 等 BUSY 进入忙电平（≤300ms，容忍命令置位延迟）；
 *   ② 等 BUSY 释放（≤busy_timeout_ms，三色波形 ~14.7s）。
 * busy 从未置位 = SPI 命令未达 COG（硬件排查现场判据） */
static void wait_refresh_done(void)
{
    const uint32_t t0 = millis();
    while (digitalRead(EPD_BUSY_PIN) != g_panel_gdew027c44.busy_level &&
           millis() - t0 < 300)
        delay(2);
    const bool asserted =
        digitalRead(EPD_BUSY_PIN) == g_panel_gdew027c44.busy_level;
    const uint32_t t1 = millis();
    while (digitalRead(EPD_BUSY_PIN) == g_panel_gdew027c44.busy_level &&
           millis() - t1 < g_panel_gdew027c44.busy_timeout_ms)
        delay(10);
    Serial.printf("[G027-DIAG] 0x12 busy: %s @%ums, active %ums\n",
                  asserted ? "LOW" : "never",
                  (unsigned)(millis() - t0), (unsigned)(millis() - t1));
}

/* —— 五组 LUT（GxEPD2_270c lut_20~24 原值，GDEW027C44 官方波形；
 * IL91874 无 OTP 三色波形，PSR 0xaf 选 register LUT 后必须显式下发） —— */
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

static void il_write_lut(uint8_t cmd, const uint8_t *lut, size_t n)
{
    epd_cmd(cmd);
    epd_write_plane(lut, n, false);   /* LUT 亦逐字节独立 CS（GxEPD2 sCS 同款） */
}

/* RAM 全屏窗口（_setPartialRamArea_270c 原格式 8 字节：x>>8, x&0xf8,
 * y>>8, y&0xff, w>>8, w&0xf8, h>>8, h&0xff；全屏 x=0 y=0 w=176 h=264） */
static void set_full_window(uint8_t cmd)
{
    epd_cmd(cmd);
    epd_dat(0x00); epd_dat(0x00);                  /* x = 0 */
    epd_dat(0x00); epd_dat(0x00);                  /* y = 0 */
    epd_dat(0x00); epd_dat((uint8_t)(g_panel_gdew027c44.panel_w & 0xF8));
                                                  /* w = 176 (0xB0) */
    epd_dat((uint8_t)(g_panel_gdew027c44.panel_h >> 8));
    epd_dat((uint8_t)(g_panel_gdew027c44.panel_h & 0xFF)); /* h = 264 (0x108) */
}

static int panel_init(void)
{
    /* GxEPD2_270c _InitDisplay + _Init_Full + _PowerOn 一比一
     * （Info/ink_test 真机验证口径） */
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
    panel_wait_idle(5000);                    /* 复位后 boot 自检（忙→闲） */

    epd_cmd(0x01);                            /* 驱动配置（gate 软起等） */
    epd_dat(0x03); epd_dat(0x00); epd_dat(0x2b); epd_dat(0x2b); epd_dat(0x09);
    epd_cmd(0x06);                            /* boost 软启动 */
    epd_dat(0x07); epd_dat(0x07); epd_dat(0x17);
    epd_cmd(0xF8); epd_dat(0x60); epd_dat(0xA5);   /* 命令解锁序列 ×5 */
    epd_cmd(0xF8); epd_dat(0x89); epd_dat(0xA5);
    epd_cmd(0xF8); epd_dat(0x90); epd_dat(0x00);
    epd_cmd(0xF8); epd_dat(0x93); epd_dat(0x2A);
    epd_cmd(0xF8); epd_dat(0x73); epd_dat(0x41);
    epd_cmd(0x16); epd_dat(0x00);             /* 增强命令复位 */
    epd_cmd(0x00); epd_dat(0xaf);             /* PSR: by register LUT */
    epd_cmd(0x30); epd_dat(0x3a);             /* PLL 90Hz */
    epd_cmd(0x61);                            /* 分辨率 176x264（竖屏原生） */
    epd_dat(0x00); epd_dat((uint8_t)g_panel_gdew027c44.panel_w);
    epd_dat((uint8_t)(g_panel_gdew027c44.panel_h >> 8));
    epd_dat((uint8_t)(g_panel_gdew027c44.panel_h & 0xFF));
    epd_cmd(0x82); epd_dat(0x12);             /* VCOM_DC */
    epd_cmd(0x50); epd_dat(0x87);             /* CDI VCOM 与边框 */

    il_write_lut(0x20, LUT_VCOM,   sizeof(LUT_VCOM));
    il_write_lut(0x21, LUT_WW,     sizeof(LUT_WW));
    il_write_lut(0x22, LUT_RED,    sizeof(LUT_RED));
    il_write_lut(0x23, LUT_WHITE,  sizeof(LUT_WHITE));
    il_write_lut(0x24, LUT_BLACK,  sizeof(LUT_BLACK));

    epd_cmd(0x04);                            /* power on */
    panel_wait_idle(5000);

    s_ready = true;
    return 0;
}

/* 双平面写入 + 真全刷（GxEPD2 writeImage + _Update_Full 一比一）：
 * planes[0] = B/W 平面（driver 语义 bit=1 白）→ 取反发 0x14（RAM 1=黑）；
 * planes[1] = 红位平面（bit=1 红）→ 直通发 0x15（RAM 1=红）；
 * 刷新 0x12 直接激活（UC/IL 系标准，非 SSD16xx 的 0x22/0x20 序列） */
static int do_refresh(const uint8_t *bw_plane, const uint8_t *red_plane)
{
    if (!s_ready && panel_init() != 0) return -1;
    const size_t plane_bytes =
        (size_t)(g_panel_gdew027c44.panel_w / 8) * g_panel_gdew027c44.panel_h;

    set_full_window(0x14);                    /* black RAM 全屏窗口 */
    epd_write_plane(bw_plane, plane_bytes, true);
    set_full_window(0x15);                    /* red RAM 全屏窗口 */
    epd_write_plane(red_plane, plane_bytes, false);

    epd_cmd(0x12);                            /* display refresh */
    wait_refresh_done();                      /* 三色波形 ~14.7s：等完成再下电 */
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
        if (!s_ready && panel_init() != 0) return -1;
        const size_t plane_bytes =
            (size_t)(g_panel_gdew027c44.panel_w / 8) * g_panel_gdew027c44.panel_h;
        set_full_window(0x14);
        for (size_t i = 0; i < plane_bytes; i++) epd_dat(0x00);
        set_full_window(0x15);
        for (size_t i = 0; i < plane_bytes; i++) epd_dat(0x00);
        epd_cmd(0x12);
        wait_refresh_done();
        return 0;
    }
    return panel_full_refresh(frame);
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
    epd_cmd(0x02);
    panel_wait_idle(g_panel_gdew027c44.busy_timeout_ms);
    s_ready = false;
}

static void panel_deep_sleep(void)
{
    /* GxEPD2_270c hibernate 一比一：0x07 check 0xA5 深睡（~µA 级），
     * RST 硬复位唤醒 + panel_init 重初始化 */
    epd_cmd(0x07); epd_dat(0xA5);
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
    .panel_w    = 176,
    .panel_h    = 264,
    .gfx_rotation = 1,       /* 物理竖屏 176x264 → UI 横屏 264x176 */
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
    .fb_location = EPD_FB_AUTO,   /* 双帧+画布 ~35KB < 128KB → 全 SRAM
                                   * （§10.2；与 WiFi/BLE 共存余量足） */
    .rst_pulse_ms = 20,           /* GxEPD2 默认复位脉宽（真机验证） */
    .busy_level = 0,              /* BUSY=LOW 忙（UC/IL 系，与
                                   * SSD16xx HIGH=忙相反） */
    .busy_timeout_ms = 20000,     /* 实测忙 14720ms + 余量 */
    .power_on_ms  = 50,           /* 0x04 后自检，保守近似值 */
    .power_off_ms = 50,
    .full_ms    = 15000,          /* 真机实测 14720ms（2026-08-22
                                   * bring-up 多轮一致）+余量 */
    .partial_ms = 15000,          /* 无快速局刷：partial==full */
    .partial_enabled = false,     /* 三色面板一律 false（§13.2） */
    .passes     = 1,
    .partial_count_full_refresh = 1, /* 无局刷：阈值不参与调度，保守 1 */
    .window_8align = true,        /* 窗口 x/w 8 像素对齐（全屏 176 ✓） */
    .ops = {
        .init         = panel_init,
        .full_refresh = panel_full_refresh,
        .write_full   = panel_write_full,
        .partial      = NULL,     /* partial_enabled=false，L3 不触达 */
        .power_off    = panel_power_off,
        .deep_sleep   = panel_deep_sleep,
        .probe        = NULL,     /* IL91874 无 FLG/版本读寄存器
                                  * （0x71 位掩读恒 0x00 而屏正常，
                                  * 2026-08-22 真机实证），无 probe 路径 */
        .write_planes = panel_write_planes,
    },
};
