/**
 * @file epd_driver.cpp
 * @brief DEPG0370BBU253F33HP-M7 3.7" 墨水屏驱动 — epd2 直驱 + 自持画布
 *
 * 硬件：ESP32-S3 + EVK011 转接板 + DEPG0370 3.7" 墨水屏（240x416, UC8253 类 COG）
 *
 * 架构（2026-08-18 残影叠加修复后确定）：
 *   - UI 绘图画布：GFXcanvas1（横屏 416x240，Adafruit GFX 完整字体栈，
 *     getBuffer() 公开可读 —— GxEPD2_BW 的 _buffer 为 private 无法取旧帧，
 *     这是放弃其 displayWindow 增量路径的直接原因）
 *   - 刷新：自持双帧（s_port_new/s_port_prev 竖屏 240x416），
 *     全刷 = writeImageForFullRefresh(双写 0x10+0x13)+refresh(false)；
 *     局刷 = demo 忠实序列：硬复位 → partial 初始化 → 双 RAM 写窗口
 *     （旧帧→0x10 差分基准，新帧→0x13）→ 0x04/0x12/0x02，
 *     完全无状态，不依赖 COG 内部 RAM 跨刷新存活
 *     （对照 Info/ 官方 demo Display_windows_image_partial_update；
 *      GxEPD2 增量路径只写 0x13、依赖 COG 0x10 持久 —— 本面板上不可靠，
 *      残影叠加根因，真机连续翻词 20+ 次字迹叠加实测）
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
#include <Adafruit_GFX.h>
#include "GxEPD2_374_DEPG0370.h"

#include <Fonts/FreeSans9pt7b.h>
#include "Fonts/Arial14pt7b.h"   /* 官方 fontconvert 从 Arial.ttf 生成（GFX 库无 14pt 档） */
#include <Fonts/FreeSans18pt7b.h>
#include <Fonts/FreeSans24pt7b.h>

#include <string.h>

/* epd2 层驱动对象（epd2 直驱，不用 GxEPD2_BW 显示层） */
static GxEPD2_374_DEPG0370 s_epd2(
    EPD_CS_PIN, EPD_DC_PIN, EPD_RESET_PIN, EPD_BUSY_PIN);

/* UI 绘图画布（横屏 416x240，堆分配，getBuffer() 公开可读） */
static GFXcanvas1 *s_canvas = NULL;

/* 自持双帧（竖屏 240x416，行宽 30 字节，bit=1 白，与 COG SRAM 语义一致）：
 * s_port_new  = 最近一次绘制转置结果（待刷新/已刷新的新帧）
 * s_port_prev = 屏幕当前真实内容快照（局刷 0x10 差分基准） */
static uint8_t s_port_new[EPD_FB_SIZE];
static uint8_t s_port_prev[EPD_FB_SIZE];

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
    return color ? GxEPD_BLACK : GxEPD_WHITE; /* 1=黑(0x00), 0=白(0xFFFF) */
}

/* 横屏画布 → 竖屏面板帧转置（对应 GxEPD2_BW setRotation(1) 顺时针 90°：
 * 像素映射 panel_x = 239 - y_gfx，panel_y = x_gfx；_reverse=false）。
 * 画布与面板同为 bit=1 白，置位直通，无需反相 */
static void canvas_to_panel(uint8_t *panel)
{
    const uint8_t *src = s_canvas->getBuffer();
    const int stride = (EPD_GFX_WIDTH + 7) / 8; /* 52 字节/行 */
    memset(panel, 0x00, EPD_FB_SIZE);
    for (int cy = 0; cy < EPD_GFX_HEIGHT; cy++) {
        const uint8_t *row = src + cy * stride;
        for (int cx = 0; cx < EPD_GFX_WIDTH; cx++) {
            if (row[cx >> 3] & (0x80 >> (cx & 7))) {
                int px = (EPD_GFX_HEIGHT - 1) - cy;
                int py = cx;
                panel[py * (EPD_WIDTH / 8) + (px >> 3)] |= (uint8_t)(0x80 >> (px & 7));
            }
        }
    }
}

/* GFX 横屏窗口 → 面板竖屏窗口（rotation=1：swap(x,y)/swap(w,h)，
 * x = 240 - x - w；与 GxEPD2_BW displayWindow 的 _rotate+._reverse 语义一致）。
 * 注：面板侧 x/w 由 epd2 层自动 8 像素对齐（对应横屏 y/h 对齐约束不变） */
static void gfx_rect_to_panel(int x, int y, int w, int h,
                              uint16_t *px, uint16_t *py, uint16_t *pw, uint16_t *ph)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > EPD_GFX_WIDTH)  w = EPD_GFX_WIDTH - x;
    if (y + h > EPD_GFX_HEIGHT) h = EPD_GFX_HEIGHT - y;
    if (w <= 0 || h <= 0) { *pw = 0; *ph = 0; return; }

    uint16_t rx = (uint16_t)y;
    uint16_t ry = (uint16_t)x;
    uint16_t rw = (uint16_t)h;
    uint16_t rh = (uint16_t)w;
    rx = (uint16_t)(EPD_GFX_HEIGHT - rx - rw);

    *px = rx; *py = ry; *pw = rw; *ph = rh;
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
    pinMode(EPD_BUSY_PIN, INPUT); /* 恢复高阻，交回驱动 */
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

    /* 4. epd2 层初始化（硬件复位 20ms，demo 时序）+ UI 画布分配 */
    s_epd2.init(0 /* 串口诊断关闭 */, true, 20, false);

    s_canvas = new GFXcanvas1(EPD_GFX_WIDTH, EPD_GFX_HEIGHT);
    if (!s_canvas || !s_canvas->getBuffer()) {
        LOG_E("canvas alloc failed (%d bytes)", EPD_GFX_WIDTH * EPD_GFX_HEIGHT / 8);
        return -1;
    }
    s_canvas->fillScreen(GxEPD_WHITE);   /* 画布白底（与旧全刷首帧行为一致） */
    s_canvas->setTextColor(GxEPD_BLACK);
    s_canvas->setFont(s_fonts[1]);
    s_canvas->setTextWrap(false);
    memset(s_port_prev, 0xFF, EPD_FB_SIZE); /* 上一帧影子初始化为白（首次全刷前防御） */

    s_inited = true;
    LOG_I("EPD driver initialized: DEPG0370 240x416, canvas+demo-partial arch (HW SPI %d/%d)",
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
    s_epd2.powerOff();
    LOG_D("EPD power OFF (0x02 sent)");
}

void epd_clear_screen(void)
{
    if (!s_inited) return;
    /* 黑白交替一轮再回白：仅白帧全刷对长时间驻留的深色像素翻转不彻底
     * （真机验证：旧布局时钟数小时局刷后，开机白屏全刷仍留残影），
     * 先全黑全刷把陈年黑迹充分翻转再回白；调用点均为低频路径
     * （开机白屏 / 长按清残影 / 局刷阈值），多一次全刷可接受 */
    s_canvas->fillScreen(GxEPD_BLACK);
    epd_gfx_flush();
    s_canvas->fillScreen(GxEPD_WHITE);
    epd_gfx_flush();
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
     * 注意：UI 主路径请用 epd_gfx_*（横屏 GFX 坐标）；
     * 本路径不更新 s_port_prev（LAN 直传后调用方须强制下一次全刷，
     * main.cpp ui_force_full_refresh_next() 已保证） */
    if (data) {
        s_epd2.writeImageForFullRefresh(data, 0, 0, EPD_WIDTH, EPD_HEIGHT);
    } else {
        s_epd2.writeScreenBuffer(0xFF);
    }
    s_epd2.refresh(false); /* 全刷 */
    s_epd2.powerOff();

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
     * 遗留直通 API（refresh_submit 用）：写 0x13 → 局刷 → 回写 0x10；
     * 不维护 s_port_prev —— UI 主路径已改走 epd_gfx_flush_window */
    s_epd2.drawImagePart(data, 0, 0, w, h, x, y, w, h);

    LOG_D("EPD partial refresh [%d,%d,%d,%d]", x, y, w, h);
}

void epd_deep_sleep(void)
{
    if (!s_inited) return;

    s_epd2.hibernate(); /* 0x02 下电 + 0x07/0xA5 深睡，可被硬件复位唤醒 */

    /* 注意：保持 s_inited=true —— hibernate 后置 _init_display_done=false，
     * 下一次局刷路径的 hwReset()/写数据前会自动复位并重新初始化 */
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
 * C-callable GFX 包装（画布直通，y 基线语义与 FreeSans 字体一致）
 * ============================================================ */

extern "C" {

int epd_gfx_width(void)  { return s_canvas ? s_canvas->width() : 0; }
int epd_gfx_height(void) { return s_canvas ? s_canvas->height() : 0; }

void epd_gfx_fill_screen(uint16_t color)
{
    if (s_canvas) s_canvas->fillScreen(gfx_color(color));
}

void epd_gfx_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (s_canvas) s_canvas->fillRect(x, y, w, h, gfx_color(color));
}

void epd_gfx_draw_rect(int x, int y, int w, int h, uint16_t color)
{
    if (s_canvas) s_canvas->drawRect(x, y, w, h, gfx_color(color));
}

void epd_gfx_draw_hline(int x, int y, int w, uint16_t color)
{
    if (s_canvas) s_canvas->drawFastHLine(x, y, w, gfx_color(color));
}

void epd_gfx_draw_vline(int x, int y, int h, uint16_t color)
{
    if (s_canvas) s_canvas->drawFastVLine(x, y, h, gfx_color(color));
}

void epd_gfx_draw_text(int x, int y, const char *text, uint16_t color, int font_size)
{
    if (!text || !s_canvas) return;

    /* FreeSans 无 CJK 字形，超出 Latin-1 的字符将无法渲染 */
    for (const char *p = text; *p; p++) {
        if ((uint8_t)*p > 0x7F) {
            LOG_W("draw_text: non-ASCII text (CJK needs U8g2 font): '%s'", text);
            break;
        }
    }

    s_canvas->setFont(font_for_size(font_size));
    s_canvas->setTextColor(gfx_color(color));
    s_canvas->setCursor(x, y); /* FreeSans: y 为基线 */
    s_canvas->print(text);
}

void epd_gfx_text_bounds(const char *text, int font_size, int *out_w, int *out_h)
{
    if (!out_w || !out_h) return;
    if (!text || !s_canvas) {
        *out_w = 0;
        *out_h = 0;
        return;
    }
    int16_t x1, y1;
    uint16_t w, h;
    s_canvas->setFont(font_for_size(font_size));
    s_canvas->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    *out_w = w;
    *out_h = h;
}

void epd_gfx_draw_bitmap(int x, int y, int w, int h, const uint8_t *bits, uint16_t color)
{
    if (!bits || !s_canvas) return;
    /* bits：行主序 MSB-first（每行 ceil(w/8) 字节），bit=1 画 color，0 透明 */
    s_canvas->drawBitmap(x, y, bits, w, h, gfx_color(color));
}

void epd_gfx_flush(void)
{
    if (!s_inited) return;
    canvas_to_panel(s_port_new);
    /* demo 忠实版真全刷（Display_image_full_update）：硬复位（清局刷残留
     * E0/E5/PSR2）→ full 初始化（PSR+CDI=0x97）→ 无窗口整屏写 0x13
     * → 0x04/0x12/0x02。注意：不能用 demoWriteDual 全屏参数代替 ——
     * 窗口包裹的全屏刷驱动力不足，真机实测留残影（2026-08-18） */
    s_epd2.hwReset();
    s_epd2.initFullDemo();
    s_epd2.demoWriteFull(s_port_new);
    s_epd2.updateDemoPartial(); /* update 序列全刷/局刷同款（0x04/0x12/0x02） */
    memcpy(s_port_prev, s_port_new, EPD_FB_SIZE); /* 屏幕内容 == 新帧 */
}

void epd_gfx_flush_window(int x, int y, int w, int h)
{
    if (!s_inited) return;

    uint16_t px, py, pw, ph;
    gfx_rect_to_panel(x, y, w, h, &px, &py, &pw, &ph);
    if (pw == 0 || ph == 0) return;

    canvas_to_panel(s_port_new);

    /* demo 忠实版局刷（对照 Display_windows_image_partial_update）：
     * 每次局刷硬复位 COG + partial 初始化（波形参数可切换）+
     * 单会话双 RAM 写窗口（旧帧→0x10，新帧→0x13）+ 0x04/0x12/0x02。
     * 完全无状态 —— 不依赖 COG 内部 RAM 跨刷新存活 */
    s_epd2.hwReset();          /* demo：每次局刷前硬件复位，COG 状态归零 */
    s_epd2.initPartialDemo();
    s_epd2.demoWriteDual(px, py, pw, ph, s_port_prev, s_port_new);
    s_epd2.updateDemoPartial(); /* demo：0x04→0x12→0x02 */

    memcpy(s_port_prev, s_port_new, EPD_FB_SIZE); /* 屏幕内容 == 新帧 */
}

} /* extern "C" for GFX wrappers */
