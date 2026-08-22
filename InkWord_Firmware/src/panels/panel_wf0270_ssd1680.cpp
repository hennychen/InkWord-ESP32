/**
 * @file panel_wf0270_ssd1680.cpp
 * @brief WEIFENG WF0270 面板单元（L0）—— SSD1680 手写序列 + 三色 desc
 *        （PANEL_COMPAT_DESIGN §十六 SOP 第三块屏 / LAYOUT_SMALL 首例）
 *
 * 面板：维峰 WEIFENG WF0270T1PCZ2200E4，2.7" 264x176 黑白红三色，
 * 22Pin 0.5mm FPC，3.3V。IC 推断 SSD1680（高置信度：同族 Solomon
 * SSD16xx，与 Waveshare 2.7" e-Paper (B) V2 同规格同分辨率；待规格书
 * 确认，bring-up 时 0x2F 版本读 + 序列响应度实锤）。
 *
 * 几何（demo SetWindows 实证，2026-08-22 核对）：COG 物理竖屏 ——
 * 源极（X，0x44 字节窗口）176px / 门极（Y，0x45 窗口）264 线，
 * panel_w=176 / panel_h=264，gfx_rotation=1 → UI 横屏 264x176
 * （同 DEPG0370 先例；layout_profile LAYOUT_SMALL 档位 §8.1 预留）。
 * !! 方案初稿 panel_w=264/panel_h=176 为笔误——源极窗口超 176 会
 * 使 0x44/0x45 与 RAM 布局转置错位（SSD1680 源极上限 176px）。
 *
 * 序列来源（权威，一比一移植）：Waveshare 2.7inch e-Paper (B) V2
 * 官方 demo epd2in7b_V2.cpp（同为 SSD1680 264x176 BWR）——Init 极简
 * （无 0x74/0x7E/0x0C/0x2B 软启动链，OTP 预载批次）、刷新直接 0x20
 * Master Activation（PSR 0x27 已含 LUT/色序配置，无需 SSD1619 的
 * 0x22/0xF7 显式序列）。与 E042A13（SSD1619，2017 批次须显式初始
 * 代码）差异见各函数注释。
 *
 * 双 RAM 语义（SSD16xx 家族一致）：0x24 B/W RAM bit=1 白 + 0x26
 * color RAM bit=1 红。demo DisplayFrame 黑平面 ~buffer 取反系其
 * image 约定（bit=0=白），本单元 plane[0] 约定 bit=1=白直通，
 * 不移植取反（ClearFrame 白=0xFF/红=0x00 证实 0x24 bit=1=白）。
 *
 * 无状态设计（desc 头注释铁律，同 E042A13）：do_refresh 前置
 * s_ready 检查，power_off/deep_sleep 后归零，下次刷新自动 RST
 * 唤醒 + 完整重配。
 */
#include "../epd_panel.h"
#include "../gpio_config.h"

#include <Arduino.h>
#include <SPI.h>

/* 前置声明：ops 实现引用 desc 几何/时序字段 */
extern const epd_panel_desc_t g_panel_wf0270;

static bool s_ready = false;   /* init 完成（power_off/deep_sleep 归零） */

/* —— SPI 底层（epd_driver_init 已 SPI.begin，此处事务直发；E042A13
 * bring-up 实证 transfer 逐字节连发，SPI.writeBytes 在 ESP32-S3
 * Arduino core 存在 RAM 不落地陷阱，禁用） —— */
static void epd_cmd(uint8_t c)
{
    SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    digitalWrite(EPD_CS_PIN, LOW);
    digitalWrite(EPD_DC_PIN, LOW);    /* DC=0 命令 */
    SPI.transfer(c);
    digitalWrite(EPD_CS_PIN, HIGH);
    SPI.endTransaction();
}

static void epd_dat(uint8_t d)
{
    SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    digitalWrite(EPD_CS_PIN, LOW);
    digitalWrite(EPD_DC_PIN, HIGH);   /* DC=1 数据 */
    SPI.transfer(d);
    digitalWrite(EPD_CS_PIN, HIGH);
    SPI.endTransaction();
}

/* 批量写 RAM（同 E042A13 实证路径：单事务 transfer 逐字节连发） */
static void epd_write_buf(const uint8_t *p, size_t n)
{
    SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    digitalWrite(EPD_CS_PIN, LOW);
    digitalWrite(EPD_DC_PIN, HIGH);
    for (size_t i = 0; i < n; i++) SPI.transfer(p[i]);
    digitalWrite(EPD_CS_PIN, HIGH);
    SPI.endTransaction();
}

/* 等 BUSY 回空闲（单段，init/关电路径用；SSD16xx HIGH=忙） */
static void panel_wait_idle(uint32_t timeout_ms)
{
    const uint32_t t0 = millis();
    while (digitalRead(EPD_BUSY_PIN) == g_panel_wf0270.busy_level &&
           millis() - t0 < timeout_ms)
        delay(10);
}

/* 0x20 刷新后 BUSY 两段式等待（诊断 + 修正，同 E042A13 bring-up 模式）：
 *   ① 等 BUSY 进入忙电平（≤300ms，容忍命令置位延迟）；
 *   ② 等 BUSY 释放（≤busy_timeout_ms，三色波形 ~15s）。
 * busy 从未置位 = 0x20 未达 COG（SPI 硬件排查判据） */
static void wait_refresh_done(void)
{
    const uint32_t t0 = millis();
    while (digitalRead(EPD_BUSY_PIN) != g_panel_wf0270.busy_level &&
           millis() - t0 < 300)
        delay(2);
    const bool asserted =
        digitalRead(EPD_BUSY_PIN) == g_panel_wf0270.busy_level;
    const uint32_t t1 = millis();
    while (digitalRead(EPD_BUSY_PIN) == g_panel_wf0270.busy_level &&
           millis() - t1 < g_panel_wf0270.busy_timeout_ms)
        delay(10);
    Serial.printf("[WF-DIAG] 0x20 busy: %s @%ums, active %ums\n",
                  asserted ? "HIGH" : "never",
                  (unsigned)(millis() - t0), (unsigned)(millis() - t1));
}

static int panel_init(void)
{
    /* Waveshare epd2in7b_V2 Init 一比一（2021 量产批次 OTP 预载，
     * 与 E042A13 的 GDEH042Z96 §4.1 显式初始代码路径不同——本屏
     * Init 仅 PSR/数据入口/窗口/光标四步，多余初始代码反而可能
     * 覆盖 OTP 标定值） */
    pinMode(EPD_RESET_PIN, OUTPUT);
    pinMode(EPD_CS_PIN, OUTPUT);
    pinMode(EPD_DC_PIN, OUTPUT);
    pinMode(EPD_BUSY_PIN, INPUT);
    digitalWrite(EPD_CS_PIN, HIGH);
    digitalWrite(EPD_DC_PIN, LOW);

    /* demo Reset()：HIGH 200ms → LOW rst_pulse_ms → HIGH 200ms
     * （保持远长于 E042A13 的 10/20/10，VCI 升压充分再 boot） */
    digitalWrite(EPD_RESET_PIN, HIGH);
    delay(200);
    digitalWrite(EPD_RESET_PIN, LOW);
    delay(g_panel_wf0270.rst_pulse_ms);
    digitalWrite(EPD_RESET_PIN, HIGH);
    delay(200);
    panel_wait_idle(5000);            /* 复位后 boot 自检（忙→闲） */

    epd_cmd(0x12);                    /* SWRESET：寄存器回 POR，VCOM 载 OTP */
    panel_wait_idle(5000);

    /* —— PSR（demo 原样 3 字节：0x27 主配置，后两字节 demo 冗余下发，
     * COG 按命令长度截断，无害保留）—— 0x27 = REG LUT + BWR 色序，
     * LUT 取 OTP 波形表（0x20 直接激活即跑全刷序列） —— */
    epd_cmd(0x00);
    epd_dat(0x27);
    epd_dat(0x01);
    epd_dat(0x00);

    epd_cmd(0x11); epd_dat(0x03);     /* 数据入口 X+ Y+ */

    /* RAM 窗口：X 字节 0..panel_w/8-1（176/8=22 → 0..21），
     * Y 0..panel_h-1（264 → 0..0x0107，demo SetWindows 同款） */
    epd_cmd(0x44);
    epd_dat(0x00);
    epd_dat((uint8_t)(g_panel_wf0270.panel_w / 8 - 1));
    epd_cmd(0x45);
    epd_dat(0x00); epd_dat(0x00);
    epd_dat((uint8_t)((g_panel_wf0270.panel_h - 1) & 0xFF));
    epd_dat((uint8_t)((g_panel_wf0270.panel_h - 1) >> 8));

    epd_cmd(0x4E); epd_dat(0x00);     /* X 计数器归零 */
    epd_cmd(0x4F); epd_dat(0x00); epd_dat(0x00);   /* Y 计数器归零 */
    panel_wait_idle(5000);

    s_ready = true;
    return 0;
}

/* 双平面写入 + 真全刷（demo DisplayFrame 一比一）：
 * planes[0] = B/W 平面（bit=1 白）→ 0x24；
 * planes[1] = 红位平面（bit=1 红）→ 0x26（直通，见文件头语义注）；
 * 刷新直接 0x20 Master Activation——SSD1680 PSR(0x27) 下内部自动
 * 跑上电/温度/LUT/显示全序列（demo DisplayFrame() 无 0x22，
 * 与 SSD1619 的 0x22/0xF7 显式序列不同） */
static int do_refresh(const uint8_t *bw_plane, const uint8_t *red_plane)
{
    if (!s_ready && panel_init() != 0) return -1;
    const size_t plane_bytes =
        (size_t)(g_panel_wf0270.panel_w / 8) * g_panel_wf0270.panel_h;

    /* 地址计数器归零（无状态保证：不依赖上次写满后的回卷状态） */
    epd_cmd(0x4E); epd_dat(0x00);
    epd_cmd(0x4F); epd_dat(0x00); epd_dat(0x00);

    epd_cmd(0x24);
    epd_write_buf(bw_plane, plane_bytes);
    epd_cmd(0x26);
    epd_write_buf(red_plane, plane_bytes);

    epd_cmd(0x20);                    /* Master Activation（PSR 全配置） */
    wait_refresh_done();              /* 三色波形 ~15s：等真实完成再下电 */
    return 0;
}

static int panel_full_refresh(const uint8_t *frame)
{
    /* frame：双平面连续布局（Phase 2 约定）：plane_count x panel_w/8
     * x panel_h 字节，[0]=B/W 白位平面、[1]=红位平面 */
    const size_t plane_bytes =
        (size_t)(g_panel_wf0270.panel_w / 8) * g_panel_wf0270.panel_h;
    return do_refresh(frame, frame + plane_bytes);
}

static int panel_write_full(const uint8_t *frame)
{
    /* 竖屏原始帧直通全刷（epd_full_refresh / LAN 语义）：frame=NULL
     * 清白（demo ClearFrame：黑 RAM 全 0xFF + 红 RAM 全 0x00 + 0x20）。
     * 注：本路径不维护 prev 帧一致性，调用方须强制下一次全刷
     * （ui_force_full_refresh_next 已保证） */
    if (!frame) {
        if (!s_ready && panel_init() != 0) return -1;
        static const uint8_t white = 0xFF, no_red = 0x00;
        const size_t plane_bytes =
            (size_t)(g_panel_wf0270.panel_w / 8) * g_panel_wf0270.panel_h;
        epd_cmd(0x4E); epd_dat(0x00);
        epd_cmd(0x4F); epd_dat(0x00); epd_dat(0x00);
        epd_cmd(0x24);
        for (size_t i = 0; i < plane_bytes; i++) epd_dat(white);
        epd_cmd(0x26);
        for (size_t i = 0; i < plane_bytes; i++) epd_dat(no_red);
        epd_cmd(0x20);
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
    /* SSD1680 标准关电序列（GxEPD2 同款）：0x22/0xC3 + 0x20。
     * 完成后归零 s_ready —— 下次刷新完整重配（无状态铁律） */
    if (!s_ready) return;
    epd_cmd(0x22); epd_dat(0xC3);
    epd_cmd(0x20);
    panel_wait_idle(g_panel_wf0270.busy_timeout_ms);
    s_ready = false;
}

static void panel_deep_sleep(void)
{
    /* demo Sleep 一比一：0x10 check 0x01 深睡（~µA 级），
     * RST 硬复位唤醒 + panel_init 重初始化 */
    epd_cmd(0x10); epd_dat(0x01);
    s_ready = false;
}

/* —— desc 注册（第三面板单元，LAYOUT_SMALL 档首例）——
 * 时序：同族估计口径（SSD1680 2.7" 三色全刷 ~15s），bring-up 后
 * 按实测回填；BUSY 极性 HIGH=忙（demo WaitUntilIdle while(busy==1)
 * 实证）；plane 语义 0x24 bit=1 白 / 0x26 bit=1 红（demo ClearFrame
 * 反证） */
const epd_panel_desc_t g_panel_wf0270 = {
    .name       = "wf0270_ssd1680",
    .controller = EPD_CTRL_SSD1680,
    .panel_w    = 176,
    .panel_h    = 264,
    .gfx_rotation = 1,        /* 物理竖屏 176x264 → UI 横屏 264x176
                               * （顺时针 90°，同 DEPG0370 先例；
                               * LAYOUT_SMALL 档 264x176 §8.1 预留） */
    .color_mode = EPD_COLOR_3C,
    .plane_count = 2,         /* B/W 白位平面 + 红位平面（§9.3） */
    .palette    = {
        /* 逻辑色 → 平面置位掩码：bit0=B/W 平面白位，bit1=红平面红位
         * （三色映射与 E042A13 一致，SSD16xx 家族 RAM 语义同源） */
        [EPD_GFX_WHITE]  = 0x01,   /* (bw=1,red=0) → 白 */
        [EPD_GFX_BLACK]  = 0x00,   /* (0,0) → 黑 */
        [EPD_GFX_ACCENT] = 0x02,   /* (0,1) → 红 */
        [EPD_GFX_AUX]    = 0x00,   /* 3C 无第四色 → 退化黑（§9.2） */
    },
    .fb_location = EPD_FB_AUTO,   /* 双帧+画布 ~34.8KB << 128KB → 全 SRAM
                                   * （5,808B x2 平面 x2 帧 x~1.5；与
                                   * WiFi/BLE 共存余量最宽裕） */
    .rst_pulse_ms = 2,            /* demo Reset() LOW 宽度（前后 HIGH
                                   * 各 200ms 在 panel_init 内） */
    .busy_level = 1,              /* BUSY=HIGH 忙（SSD16xx 系，demo
                                   * while(busy==1) 实证） */
    .busy_timeout_ms = 20000,     /* 同族保守值，实测后回填 */
    .power_on_ms  = 40,           /* 上电含于 0x20 激活序列，近似值 */
    .power_off_ms = 30,
    .full_ms    = 15000,          /* SSD1680 2.7" 三色同族 ~15s 估计，
                                   * bring-up 实测后回填 */
    .partial_ms = 16000,          /* 无快速局刷：partial==full */
    .partial_enabled = false,     /* 三色面板一律 false（§13.2 厂商级确认） */
    .passes     = 1,
    .partial_count_full_refresh = 1, /* 无局刷：阈值不参与调度，保守 1 */
    .window_8align = true,        /* 0x44 窗口 x 8 像素对齐（176/8=22 ✓） */
    .ops = {
        .init         = panel_init,
        .full_refresh = panel_full_refresh,
        .write_full   = panel_write_full,
        .partial      = NULL,     /* partial_enabled=false，L3 不触达 */
        .power_off    = panel_power_off,
        .deep_sleep   = panel_deep_sleep,
        .probe        = NULL,     /* SSD1680 0x2F 版本读可作 probe
                                   * （epd_driver 诊断路径已覆盖），
                                   * 留 §14.3 接入 */
        .write_planes = panel_write_planes,
    },
};
