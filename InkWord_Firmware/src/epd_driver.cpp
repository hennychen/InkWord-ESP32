/**
 * @file epd_driver.cpp
 * @brief DEPG0370BBU253F33HP-M7 3.7" 墨水屏驱动 — GxEPD2 适配层
 *
 * 硬件：ESP32-S3 + EVK011 转接板 + DEPG0370 3.7" 墨水屏（240x416, UC8253 类 COG）
 *
 * 架构（2026-08 重新分析后确定）：
 *   - 驱动核心：GxEPD2_BW<GxEPD2_374_DEPG0370>（硬件 SPI 10MHz，Adafruit GFX 绘图栈）
 *   - 升压：EVK011 板上分立 boost 由屏幕 COG 从 FPC pin2(GDR) 自主驱动，
 *     MCU 仅经 J2-16 提供 VCI 3.3V —— 不输出任何 GDR/RESE 信号
 *   - 信号：BS1=LOW(4线SPI)，BUSY=LOW 忙，全刷 CDI=0x97，局刷 CDI=0x17
 *
 * 本文件提供 C API（epd_gfx_* 系列供 .c 模块使用），
 * 文字渲染使用 FreeSans 矢量字体（setCursor 的 y 为文本基线）。
 */

#include "epd_driver.h"
#include "gpio_config.h"

#include <Arduino.h>
#include <SPI.h>
#include "debug_log.h"   /* 必须在 Arduino.h 之后：还原被 esp32-hal-log 劫持的 ESP_LOGx */
#include <GxEPD2_BW.h>
#include "GxEPD2_374_DEPG0370.h"

#include <Fonts/FreeSans9pt7b.h>
#include "Fonts/Arial14pt7b.h"   /* 官方 fontconvert 从 Arial.ttf 生成（GFX 库无 14pt 档） */
#include <Fonts/FreeSans18pt7b.h>
#include <Fonts/FreeSans24pt7b.h>

/* GxEPD2 显示对象（full-buffer 模式，1bpp 12480 字节在 .bss） */
static GxEPD2_BW<GxEPD2_374_DEPG0370, GxEPD2_374_DEPG0370::HEIGHT> s_display(
    GxEPD2_374_DEPG0370(EPD_CS_PIN, EPD_DC_PIN, EPD_RESET_PIN, EPD_BUSY_PIN));

static bool s_inited = false;

static const char *TAG = "EPD"; /* debug_log 宏依赖 */

/* font_size: 1=小(9pt) 2=中(14pt,默认) 3=大(18pt) 4=特大(24pt)
 * 注：14pt 档为 Arial14pt7b（Helvetica 风格，与 FreeSans 视觉一致） */
static const GFXfont *s_fonts[] = {
    &FreeSans9pt7b,
    &Arial14pt7b,
    &FreeSans18pt7b,
    &FreeSans24pt7b,
};

static const GFXfont *font_for_size(int font_size)
{
    if (font_size < 1 || font_size > 4) font_size = 2;
    return s_fonts[font_size - 1];
}

static uint16_t gfx_color(uint16_t color)
{
    return color ? GxEPD_BLACK : GxEPD_WHITE; /* 1=黑(0x00), 0=白(0xFF) */
}

/* ============================================================
 * 公共 C API 实现
 * ============================================================ */

extern "C" {

int epd_driver_init(void)
{
    if (s_inited) {
        LOG_W("epd_driver already initialized, skip");
        return 0;
    }

    /* 1. BS1=LOW 选择 4 线 SPI 模式（EVK011 J2-10）。
     *    省线方案：在转接板侧将 J2-10 直接短接 GND（板上就近接 J2-1），
     *    并把 gpio_config.h 的 EPD_BS_PIN 改为 -1 —— 硬接 GND 比 GPIO 驱动
     *    更稳（ESP32 启动前 ~100ms 该脚高阻，硬接 GND 无采样不定窗口） */
#if EPD_BS_PIN >= 0
    pinMode(EPD_BS_PIN, OUTPUT);
    digitalWrite(EPD_BS_PIN, LOW);
#endif

    /* 2. BUSY 三态诊断：
     *    a) 高阻输入读电平：COG 空闲时应为 1
     *    b) 开内部上拉(≈45kΩ)再读：区分「悬空断线」与「被驱动为低」
     *       - 上拉后变 1 → 线悬空：BUSY 断线/接错针/FPC 未连通
     *       - 上拉后仍 0 → 有源驱动低：COG 在拉低（或线对地短路） */
    pinMode(EPD_BUSY_PIN, INPUT);
    Serial.printf("[EPD-DIAG] pins: BS=%d SCK=%d MOSI=%d DC=%d CS=%d RST=%d BUSY=%d\n",
                  EPD_BS_PIN, EPD_SCK_PIN, EPD_MOSI_PIN, EPD_DC_PIN,
                  EPD_CS_PIN, EPD_RESET_PIN, EPD_BUSY_PIN);
    int b_float = digitalRead(EPD_BUSY_PIN);
    pinMode(EPD_BUSY_PIN, INPUT_PULLUP);
    delay(5);
    int b_pulled = digitalRead(EPD_BUSY_PIN);
    pinMode(EPD_BUSY_PIN, INPUT); /* 恢复高阻，交回 GxEPD2 */
    const char *busy_verdict = (b_float == 1) ? "HIGH (idle — or floating, see RST test below)"
                               : (b_pulled == 1) ? "FLOATING (open wire / wrong pin / FPC not seated)"
                               : "DRIVEN LOW (COG busy, or short to GND)";
    Serial.printf("[EPD-DIAG] BUSY hi-Z: %d | with-pullup: %d -> %s\n",
                  b_float, b_pulled, busy_verdict);

    /* 2b. RST 复位脉冲测试（决定性，区分「COG 真活着」与「BUSY 悬空浮高」）：
     *     COG 复位后自检会主动拉低 BUSY 一段时间。
     *     出现低电平 → COG 供电+GND+BUSY 线全通（浮空线绝不会有此反应）
     *     无反应     → 屏断电(VCI/GND)/FPC 未插/BUSY 线断/RST 线断 */
    pinMode(EPD_RESET_PIN, OUTPUT);
    digitalWrite(EPD_RESET_PIN, HIGH);
    delay(10);
    digitalWrite(EPD_RESET_PIN, LOW);
    delay(20);                            /* 复位脉宽 >10ms */
    int rst_saw_low = 0, t_low_ms = -1;
    digitalWrite(EPD_RESET_PIN, HIGH);    /* 释放复位，COG boot */
    for (int i = 0; i < 400; i++) {       /* 400ms 窗口，1ms 采样 */
        if (digitalRead(EPD_BUSY_PIN) == 0) { rst_saw_low = 1; t_low_ms = i; break; }
        delay(1);
    }
    Serial.printf("[EPD-DIAG] RST pulse -> BUSY went LOW: %d (@%dms) %s\n",
                  rst_saw_low, t_low_ms,
                  rst_saw_low ? "-> COG ALIVE: VCI/GND/BUSY/RST all wired"
                              : "-> NO RESPONSE: check VCI 3V3 / GND / FPC / BUSY wire / RST wire");

    /* 3. 硬件 SPI（EVK011 J2: SCK=pin3, SDO=pin5）。
     * GxEPD2 内部 SPI.beginTransaction 使用 GPIO matrix，任意引脚可用 */
    SPI.begin(EPD_SCK_PIN, -1, EPD_MOSI_PIN, EPD_CS_PIN);

    /* 4. GxEPD2 初始化：硬件复位 20ms（demo 时序），initial=true 触发首次全刷 */
    s_display.init(0 /* 串口诊断关闭 */, true, 20, false);
    s_display.setRotation(1); /* 横屏 416x240（rotation 1=顺时针 90°，方向不适改 3） */
    s_display.setTextColor(GxEPD_BLACK);
    s_display.setFont(s_fonts[1]);
    s_display.setTextWrap(false);

    s_inited = true;
    LOG_I("EPD driver initialized: DEPG0370 240x416 via GxEPD2 (HW SPI %d/%d MHz-capable)",
          EPD_SCK_PIN, EPD_MOSI_PIN);
    LOG_I("Booster: EVK011 discrete boost driven by panel COG (GDR on FPC pin2), no MCU PWM");
    return 0;
}

void epd_power_on(void)
{
    /* 电源由屏幕 COG 在 0x04 命令后自主升压，此处仅保证 SPI/引脚就绪 */
    if (!s_inited) return;
    LOG_D("EPD power ON requested (panel COG self-managed)");
}

void epd_power_off(void)
{
    if (!s_inited) return;
    s_display.epd2.powerOff();
    LOG_D("EPD power OFF (0x02 sent)");
}

void epd_clear_screen(void)
{
    if (!s_inited) return;
    /* 黑白交替一轮再回白：仅白帧全刷对长时间驻留的深色像素翻转不彻底
     * （真机验证：旧布局时钟数小时局刷后，开机白屏全刷仍留残影），
     * 先全黑全刷把陈年黑迹充分翻转再回白；调用点均为低频路径
     * （开机白屏 / 长按清残影 / 局刷阈值），多一次全刷可接受 */
    s_display.fillScreen(GxEPD_BLACK);
    s_display.display(false);
    s_display.fillScreen(GxEPD_WHITE);
    s_display.display(false);
    LOG_D("EPD deep clear done (black-white cycle)");
}

void epd_full_refresh(const uint8_t *data)
{
    if (!s_inited) {
        LOG_E("EPD not initialized");
        return;
    }

    /* data：竖屏整帧（面板物理 240x416，行宽 30，bit=1 白/0=黑 COG 原生语义），
     * 直通 epd2 层不做旋转；NULL 时清白。
     * 注意：UI 主路径请用 epd_gfx_*（横屏 GFX 坐标） */
    if (data) {
        s_display.epd2.writeImageForFullRefresh(data, 0, 0, EPD_WIDTH, EPD_HEIGHT);
    } else {
        s_display.epd2.writeScreenBuffer(0xFF);
    }
    s_display.epd2.refresh(false); /* 全刷 */
    s_display.epd2.powerOff();

    LOG_D("EPD full refresh done");
}

void epd_partial_refresh(int x, int y, int w, int h, const uint8_t *data)
{
    if (!s_inited) {
        LOG_E("EPD not initialized");
        return;
    }
    if (!data) {
        LOG_E("EPD partial refresh: data is NULL");
        return;
    }

    /* data 为窗口位图（竖屏面板坐标，行宽 ceil(w/8)，bit=1 白/0x00 黑），
     * drawImagePart 直通 epd2 层：写 0x13 → 局刷 → 回写 0x10（previous），
     * 与 demo EPD_Dis_Part_RAM 语义一致；x/w 自动 8 像素对齐 */
    s_display.epd2.drawImagePart(data, 0, 0, w, h, x, y, w, h);

    LOG_D("EPD partial refresh [%d,%d,%d,%d]", x, y, w, h);
}

void epd_deep_sleep(void)
{
    if (!s_inited) return;

    s_display.hibernate(); /* 0x02 下电 + 0x07/0xA5 深睡，可被硬件复位唤醒 */

    /* 注意：保持 s_inited=true —— hibernate 后 GxEPD2 置 _init_display_done=false，
     * 下一次写数据/刷新前会自动 _reset() 并重新初始化，无需重新 epd_driver_init() */
    LOG_I("EPD entered deep sleep (wake by reset)");
}

uint16_t epd_get_manufacturer(char *manufacturer, size_t len)
{
    if (manufacturer && len > 0) {
        snprintf(manufacturer, len, "DEPG");
    }
    return 0x0370;
}

} /* extern "C" */

/* ============================================================
 * C-callable GFX 包装（Adafruit GFX 直通，y 基线语义与 FreeSans 字体一致）
 * ============================================================ */

extern "C" {

int epd_gfx_width(void)  { return s_display.width(); }
int epd_gfx_height(void) { return s_display.height(); }

void epd_gfx_fill_screen(uint16_t color)
{
    s_display.fillScreen(gfx_color(color));
}

void epd_gfx_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    s_display.fillRect(x, y, w, h, gfx_color(color));
}

void epd_gfx_draw_rect(int x, int y, int w, int h, uint16_t color)
{
    s_display.drawRect(x, y, w, h, gfx_color(color));
}

void epd_gfx_draw_hline(int x, int y, int w, uint16_t color)
{
    s_display.drawFastHLine(x, y, w, gfx_color(color));
}

void epd_gfx_draw_vline(int x, int y, int h, uint16_t color)
{
    s_display.drawFastVLine(x, y, h, gfx_color(color));
}

void epd_gfx_draw_text(int x, int y, const char *text, uint16_t color, int font_size)
{
    if (!text) return;

    /* FreeSans 无 CJK 字形，超出 Latin-1 的字符将无法渲染 */
    for (const char *p = text; *p; p++) {
        if ((uint8_t)*p > 0x7F) {
            LOG_W("draw_text: non-ASCII text (CJK needs U8g2 font): '%s'", text);
            break;
        }
    }

    s_display.setFont(font_for_size(font_size));
    s_display.setTextColor(gfx_color(color));
    s_display.setCursor(x, y); /* FreeSans: y 为基线 */
    s_display.print(text);
}

void epd_gfx_text_bounds(const char *text, int font_size, int *out_w, int *out_h)
{
    if (!out_w || !out_h) return;
    if (!text) {
        *out_w = 0;
        *out_h = 0;
        return;
    }
    int16_t x1, y1;
    uint16_t w, h;
    s_display.setFont(font_for_size(font_size));
    s_display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    *out_w = w;
    *out_h = h;
}

void epd_gfx_draw_bitmap(int x, int y, int w, int h, const uint8_t *bits, uint16_t color)
{
    if (!bits) return;
    /* bits：行主序 MSB-first（每行 ceil(w/8) 字节），bit=1 画 color，0 透明 */
    s_display.drawBitmap(x, y, bits, w, h, gfx_color(color));
}

void epd_gfx_flush(void)
{
    if (!s_inited) return;
    s_display.display(false); /* GFX 全缓冲 → 全刷 */
}

void epd_gfx_flush_window(int x, int y, int w, int h)
{
    if (!s_inited) return;
    s_display.displayWindow(x, y, w, h); /* 差分局刷（自动维护 previous） */
}

} /* extern "C" for GFX wrappers */
