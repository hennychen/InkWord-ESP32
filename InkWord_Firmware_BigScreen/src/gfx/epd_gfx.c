/**
 * @file epd_gfx.c
 * @brief 大屏绘图层实现（epd_gfx.h；API 形状借小屏，屏协议全按
 *        lan_image.c 真机验收路径重写）
 *
 * 画布：PSRAM 1bpp（bit=1 黑 / 0 白），行宽 W/8 字节 MSB-first——
 * draw_bitmap/read_window 直接memcpy 语义（行字节对齐格式）。
 * flush：逐字节查表 8 像素 → 4bpp epdiy fb（偶 x 低半字节/奇 x 高半字节，
 * 15=白 0=黑，与 lan_image / lan_page.h 打包协议一致）→ GC16 全刷。
 *
 * 电源：常驻上电（panel_es108fc_safe_init 上电后不关），对齐
 * lan_image.c L1152-1159 真机验收模式（run99/run110）。小屏 COG
 * 的「刷新前 power on / 刷新后 deep sleep」语义不适用本屏：
 * run48/49 实证频繁断电上电致屏不稳定甚至黑屏，且并行总线无
 * COG 深睡命令可发，per-flush 断电只引入升压时序扰动。
 */
#include "epd_gfx.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/panel_es108fc.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "FreeSans12pt7b.h"
#include "FreeSans18pt7b.h"
#include "FreeSans24pt7b.h"
#include "FreeSans9pt7b.h"
#include "FreeSansBold12pt7b.h"
#include "FreeSansBold18pt7b.h"
#include "FreeSansBold24pt7b.h"
#include "FreeSansBold9pt7b.h"
#include "gfxfont.h"
#include "Helv36pt7b.h"
#include "Helv48pt7b.h"
#include "HelvBold36pt7b.h"
#include "HelvBold48pt7b.h"
#include "waveform_scanq.h"

#ifdef BIGSCREEN_APP
#include "button_handler.h"
#endif

static const char *TAG = "gfx";

/* 刷新窗口挂起按键扫描：面板扫描/升压把 GPIO19(ADC2_CH8) 分压读数压进
 * 按键窗口，去抖状态机 1.5s 后读成幻影长按（真机开机首刷实证）。
 * 只有 BIGSCREEN_APP 编入按键扫描，其余 env 空操作 */
static inline void btn_guard_begin(void)
{
#ifdef BIGSCREEN_APP
    button_scan_pause();
#endif
}

static inline void btn_guard_end(void)
{
#ifdef BIGSCREEN_APP
    button_scan_resume();
#endif
}

#define GFX_W 1920
#define GFX_H 1080
#define CANVAS_STRIDE (GFX_W / 8) /* 240 B/行（宽 8 整除，无行尾填充） */
#define CANVAS_BYTES (CANVAS_STRIDE * GFX_H)

/* 字体分派表（font_size 1~6 → 9/12/18/24/36/48pt；bold 独立一列，
 * set_bold 切换——与小屏 epd_driver.cpp 同构。5/6 档 2026-09-17 UI
 * 重设计增：Helv 表由系统 Helvetica 生成（tools/gen_gfx_font.py，
 * 与 FreeSans 同为 Helvetica 风格，混排视觉连续） */
static const GFXfont *const s_fonts[2][6] = {
    {&FreeSans9pt7b, &FreeSans12pt7b, &FreeSans18pt7b, &FreeSans24pt7b,
     &Helv36pt7b, &Helv48pt7b},
    {&FreeSansBold9pt7b, &FreeSansBold12pt7b, &FreeSansBold18pt7b,
     &FreeSansBold24pt7b, &HelvBold36pt7b, &HelvBold48pt7b},
};
static bool s_bold = false;

static uint8_t *s_canvas = NULL; /* PSRAM 1bpp 画布 */

/* 1 字节 → 4bpp 四字节展开表（bit=1 黑 0=白：0x0=黑 0xF=白）。
 * fb 布局（bring-up 文档 §12.2/§14.2 实证协议）：每字节 2 像素，
 * 偶 x=低半字节 / 奇 x=高半字节，白=15 / 黑=0 —— 画布 1 字节 8 像素
 * 须展开为 4 个 fb 字节（8 nibble）＝ 1 项 uint32_t（小端低字节
 * 先行，与 fb 线性序一致）；错半张屏/字形混叠皆源于此口径。 */
static uint32_t s_expand[256];

/* ---- 全刷波形选择（scanq 10+5，2026-09-18 调参定稿）----
 * 调参测试（bigscreen-tune 10 组预设）结论：SQ 10+5 画质最优。
 * scanq：nsat=10（黑饱和 10 扫≈1.1s）+ mmax=5（白修复 5 扫≈0.55s），
 * 共 15 相位≈2.26s。较 binfast 3+3（6 相位≈0.66s）慢但灰阶/边界更清晰。
 * 本画布 1bit 内容为主，但 scanq 的灰阶过渡更细腻（文字边缘抗锯齿）。
 * 实现：hl->waveform 常驻设为 scanq，full_update_gc16 直接使用。
 * 串口命令 w 可切换 binfast 对照（临时替换 hl->waveform）。
 */
static bool s_binfast = false;  // 默认 scanq 10+5（调参定稿）

void epd_gfx_set_binfast(bool on)
{
    s_binfast = on;
    // 更新 hl->waveform（scanq 常驻，binfast 由 full_update_gc16 临时替换）
    EpdiyHighlevelState* hl = panel_es108fc_hl();
    if (hl && !s_binfast) {
        hl->waveform = scanq_waveform();  // nsat=10, mmax=5
    }
    ESP_LOGW(TAG, "全刷波形 -> %s", s_binfast ? "binfast(3+3)" : "scanq(10+5)");
}

bool epd_gfx_binfast_enabled(void) { return s_binfast; }

/* GC16 全刷统一入口：按 s_binfast 选择波形（见上方注释）。MODE_GC16
 * 请求在 binfast 描述（type=2）下同样命中——波形查找按 mode type
 * 匹配，与 builtin 的 GC16 入口同构 */
static enum EpdDrawError full_update_gc16(EpdiyHighlevelState *hl)
{
    const EpdWaveform *orig = NULL;
    if (s_binfast) {
        orig = hl->waveform;
        hl->waveform = binfast_waveform();
    }
    enum EpdDrawError err;
    btn_guard_begin();
    err = epd_hl_update_screen(hl, MODE_GC16, epd_ambient_temperature());
    btn_guard_end();
    if (orig != NULL) {
        hl->waveform = orig;
    }
    return err;
}

int epd_gfx_init(void)
{
    if (s_canvas) {
        memset(s_canvas, 0, CANVAS_BYTES); /* 重入：整屏置白 */
        return 0;
    }
    s_canvas = heap_caps_malloc(CANVAS_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_canvas) {
        ESP_LOGE(TAG, "1bpp 画布分配失败（%d B PSRAM）", (int)CANVAS_BYTES);
        return -1;
    }
    memset(s_canvas, 0, CANVAS_BYTES);
    /* bit=1 黑(0x0) / bit=0 白(0xF)：像素 j（j=0 为字节最左）落入
     * fb 字节 j/2 的低/高半字节（j 偶/奇），即 uint32 内偏移
     * (j/2)*8 + (j&1)*4 —— 4 字节正好零损失承载 8 像素 */
    for (int b = 0; b < 256; b++) {
        uint32_t v = 0;
        for (int bit = 0; bit < 8; bit++) {
            uint32_t nib = (b & (0x80 >> bit)) ? 0x0 : 0xF;
            v |= nib << ((bit >> 1) * 8 + (bit & 1) * 4);
        }
        s_expand[b] = v;
    }
    // 初始化默认波形：scanq 10+5（调参定稿，2026-09-18）
    EpdiyHighlevelState* hl = panel_es108fc_hl();
    if (hl && !s_binfast) {
        hl->waveform = scanq_waveform();  // nsat=10, mmax=5, 15 相位
    }
    ESP_LOGI(TAG, "绘图层就绪：%dx%d，1bpp 画布 %d B，全刷波形=%s",
             GFX_W, GFX_H, (int)CANVAS_BYTES,
             s_binfast ? "binfast(3+3)" : "scanq(10+5)");
    return 0;
}

int epd_gfx_width(void) { return GFX_W; }
int epd_gfx_height(void) { return GFX_H; }

/* ---- 像素原语（clip 后直接写位；color 0=白/1=黑） ---- */
static inline void put_pixel(int x, int y, uint16_t color)
{
    if ((unsigned)x >= GFX_W || (unsigned)y >= GFX_H) return;
    uint8_t *row = s_canvas + y * CANVAS_STRIDE + (x >> 3);
    uint8_t mask = 0x80 >> (x & 7);
    if (color)
        *row |= mask;
    else
        *row &= (uint8_t)~mask;
}

void epd_gfx_fill_screen(uint16_t color)
{
    if (!s_canvas) return;
    memset(s_canvas, color ? 0xFF : 0x00, CANVAS_BYTES);
}

void epd_gfx_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_canvas || w <= 0 || h <= 0) return;
    /* 裁剪到画布 */
    int x1 = x < 0 ? 0 : x, y1 = y < 0 ? 0 : y;
    int x2 = x + w > GFX_W ? GFX_W : x + w;
    int y2 = y + h > GFX_H ? GFX_H : y + h;
    if (x1 >= x2 || y1 >= y2) return;

    for (int ry = y1; ry < y2; ry++) {
        uint8_t *row = s_canvas + ry * CANVAS_STRIDE;
        /* 首尾部分字节逐位，中段整字节 memset（宽 8 整除行无错位） */
        int cx = x1;
        while (cx < x2 && (cx & 7)) {
            uint8_t mask = 0x80 >> (cx & 7);
            if (color)
                row[cx >> 3] |= mask;
            else
                row[cx >> 3] &= (uint8_t)~mask;
            cx++;
        }
        int full_end = x2 & ~7; /* 中段整字节覆盖 [cx, full_end) */
        if (cx < full_end) {
            memset(row + (cx >> 3), color ? 0xFF : 0x00,
                   (size_t)((full_end - cx) >> 3));
            cx = full_end;
        }
        while (cx < x2) {
            uint8_t mask = 0x80 >> (cx & 7);
            if (color)
                row[cx >> 3] |= mask;
            else
                row[cx >> 3] &= (uint8_t)~mask;
            cx++;
        }
    }
}

void epd_gfx_draw_rect(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    epd_gfx_draw_hline(x, y, w, color);             /* 上 */
    epd_gfx_draw_hline(x, y + h - 1, w, color);     /* 下 */
    epd_gfx_draw_vline(x, y, h, color);             /* 左 */
    epd_gfx_draw_vline(x + w - 1, y, h, color);     /* 右 */
}

void epd_gfx_draw_hline(int x, int y, int w, uint16_t color)
{
    if (w <= 0) return;
    epd_gfx_fill_rect(x, y, w, 1, color);
}

void epd_gfx_draw_vline(int x, int y, int h, uint16_t color)
{
    if (!s_canvas || h <= 0) return;
    for (int i = 0; i < h; i++) put_pixel(x, y + i, color);
}

void epd_gfx_set_bold(bool on) { s_bold = on; }

/* ---- 文字（Adafruit GFX classic drawChar 位流算法移植） ---- */
static const GFXfont *font_for_size(int font_size)
{
    int idx = font_size < 1 ? 1 : (font_size > 6 ? 6 : font_size) - 1;
    return s_fonts[s_bold ? 1 : 0][idx];
}

void epd_gfx_draw_text(int x, int y, const char *text, uint16_t color,
                       int font_size)
{
    if (!s_canvas || !text) return;
    const GFXfont *f = font_for_size(font_size);

    int cx = x;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        unsigned char c = *p;
        if (c == '\n') { /* 换行：x 归零语义在调用方自理，这里跳过 */
            continue;
        }
        if (c < f->first || c > f->last) { /* 非 ASCII/未收录：前进跳过 */
            cx += f->glyph[0].xAdvance;
            continue;
        }
        const GFXglyph *g = &f->glyph[c - f->first];
        const uint8_t *bits = f->bitmap + g->bitmapOffset;
        int bo = 0; /* 位计数：跨行累计（连续位流，见 gfxfont.h） */
        for (int yy = 0; yy < g->height; yy++) {
            for (int xx = 0; xx < g->width; xx++) {
                if (bits[bo >> 3] & (0x80 >> (bo & 7)))
                    put_pixel(cx + g->xOffset + xx, y + g->yOffset + yy,
                              color);
                bo++;
            }
        }
        cx += g->xAdvance;
    }
}

void epd_gfx_text_bounds(const char *text, int font_size, int *out_w,
                         int *out_h)
{
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (!s_canvas || !text) return;
    const GFXfont *f = font_for_size(font_size);

    /* Adafruit getTextBounds 简化（单行、无换行）：累加 xAdvance，
     * 包围盒含首字形 xOffset 与末字形宽度修正 */
    int minx = 0, maxx = 0, miny = 0, maxy = -1, cx = 0, any = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        unsigned char c = *p;
        if (c == '\n') continue;
        if (c < f->first || c > f->last) continue;
        const GFXglyph *g = &f->glyph[c - f->first];
        int x1 = cx + g->xOffset, y1 = g->yOffset;
        int x2 = x1 + g->width, y2 = y1 + g->height;
        if (!any) {
            minx = x1; maxx = x2; miny = y1; maxy = y2; any = 1;
        } else {
            if (x1 < minx) minx = x1;
            if (x2 > maxx) maxx = x2;
            if (y1 < miny) miny = y1;
            if (y2 > maxy) maxy = y2;
        }
        cx += g->xAdvance;
    }
    if (!any) return;
    if (out_w) *out_w = maxx - minx;
    if (out_h) *out_h = maxy - miny;
}

void epd_gfx_draw_bitmap(int x, int y, int w, int h, const uint8_t *bits,
                         uint16_t color)
{
    if (!s_canvas || !bits || w <= 0 || h <= 0) return;
    int stride = (w + 7) >> 3;
    for (int ry = 0; ry < h; ry++) {
        const uint8_t *row = bits + ry * stride;
        for (int rx = 0; rx < w; rx++)
            if (row[rx >> 3] & (0x80 >> (rx & 7)))
                put_pixel(x + rx, y + ry, color);
    }
}

void epd_gfx_read_window(int x, int y, int w, int h, uint8_t *out)
{
    if (!s_canvas || !out || w <= 0 || h <= 0) return;
    if (x < 0 || y < 0 || x + w > GFX_W || y + h > GFX_H) return;
    int stride = (w + 7) >> 3;
    for (int ry = 0; ry < h; ry++) {
        const uint8_t *src = s_canvas + (y + ry) * CANVAS_STRIDE;
        uint8_t *dst = out + ry * stride;
        memset(dst, 0, (size_t)stride);
        for (int rx = 0; rx < w; rx++)
            if (src[(x + rx) >> 3] & (0x80 >> ((x + rx) & 7)))
                dst[rx >> 3] |= 0x80 >> (rx & 7);
    }
}

/* ---- 刷新（1bpp → epdiy 4bpp → GC16） ---- */
void epd_gfx_flush(void)
{
    EpdiyHighlevelState *hl = panel_es108fc_hl();
    if (!hl || !s_canvas) {
        ESP_LOGE(TAG, "flush 前置缺失：hl=%p canvas=%p", (void *)hl,
                 (void *)s_canvas);
        return;
    }
    uint8_t *fb = epd_hl_get_framebuffer(hl);
    /* 展开：每画布字节 → fb 四字节（8 nibble）；4×CANVAS_BYTES ==
     * W/2×H = fb 全尺寸，逐行恰好铺满，无残区（前版 uint16 表只
     * 覆盖 fb 前 50%，上半屏两行拼接/下半屏残留即花屏真源） */
    const uint32_t *ex = s_expand;
    uint32_t *dst = (uint32_t *)fb;
    const uint8_t *src = s_canvas;
    for (size_t i = 0; i < CANVAS_BYTES; i++) dst[i] = ex[src[i]];

    /* 电源已由 panel_es108fc_safe_init 常驻上电（对齐 lan_image
     * L1152-1159），刷新路径不碰电源开关 */
    int64_t t0 = esp_timer_get_time();
    /* epdiy 无 typedef：EpdDrawError 为 enum tag（epdiy.h L126） */
    enum EpdDrawError err = full_update_gc16(hl);
    int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;
    /* 电源保持常驻（对齐 lan_image 定稿路径）：此处不断电 */
    // 波形名：binfast 临时替换，否则用 hl->waveform（scanq 或 builtin）
    const char* wf_name = s_binfast ? "binfast" :
                          (hl->waveform && hl->waveform->num_modes > 0 ? "hl->waveform" : "builtin");
    if (err != EPD_DRAW_SUCCESS) {
        ESP_LOGE(TAG, "全刷失败 err=%d（%s，%lld ms）", (int)err,
                 wf_name, (long long)dt_ms);
    } else {
        ESP_LOGI(TAG, "全刷完成（%s，%lld ms）", wf_name, (long long)dt_ms);
    }
}
/* 灰染重置节拍（run91 定稿 K=8：局刷 nop 槽泵电荷累积为灰染阈值的
 * 1/6，重置帧强驱覆写；K 放大须有实测照片证据，不得纸面推断） */
#define PARTIAL_RESET_K 8
static int s_partial_count = 0;

/* 窗口局部刷新（run88 P4 路径）：画布区域 → fb 区域展开 →
 * panel 层 DU 窗口扫描。x/w 内部对齐到 8 像素（uint32 展开粒度，
 * 多刷边缘无害）。每 K 次局刷自动插入全屏 GC16 重置（驱白灰染） */
void epd_gfx_flush_window(int x, int y, int w, int h)
{
    EpdiyHighlevelState *hl = panel_es108fc_hl();
    if (!hl || !s_canvas) {
        ESP_LOGE(TAG, "flush_window 前置缺失：hl=%p canvas=%p", (void *)hl,
                 (void *)s_canvas);
        return;
    }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > GFX_W)  w = GFX_W - x;
    if (y + h > GFX_H) h = GFX_H - y;
    if (w <= 0 || h <= 0) return;

    int x0 = x & ~7;               /* 向下对齐 8：fb uint32 展开粒度 */
    int x1 = (x + w + 7) & ~7;     /* 向上对齐 8（边缘多刷无害） */

    uint8_t *fb = epd_hl_get_framebuffer(hl);
    const uint32_t *ex = s_expand;
    int nb = (x1 - x0) >> 3;       /* 窗口内画布字节数 */
    int64_t t0 = esp_timer_get_time();
    for (int ry = y; ry < y + h; ry++) {
        const uint8_t *srow = s_canvas + ry * CANVAS_STRIDE + (x0 >> 3);
        uint32_t *drow = (uint32_t *)(fb + (size_t)(GFX_W / 2) * ry)
                         + (x0 >> 3);  /* x0/8：每 uint32 承载 8 像素 */
        for (int i = 0; i < nb; i++) drow[i] = ex[srow[i]];
    }

    esp_err_t err;
    btn_guard_begin();
    err = panel_es108fc_update_area(x0, y, x1 - x0, h);
    btn_guard_end();
    int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DU 局刷失败 err=%d（x=%d y=%d w=%d h=%d，%lld ms）",
                 (int)err, x0, y, x1 - x0, h, (long long)dt_ms);
        return;
    }
    ESP_LOGI(TAG, "DU 局刷完成（x=%d y=%d w=%d h=%d，%lld ms，%d/%d）",
             x0, y, x1 - x0, h, (long long)dt_ms,
             s_partial_count + 1, PARTIAL_RESET_K);

    /* run91 灰染重置：第 K 次局刷后插入深清（run115 序列：binfast 自驱
     * 白→黑→白→图，DU 灰染 + 单次 force_refresh 驱不到的深滞留一并
     * 清除；2026-09-17 由 force_refresh 升级） */
    if (++s_partial_count >= PARTIAL_RESET_K) {
        s_partial_count = 0;
        epd_gfx_deep_clean();
    }
}

/* 强制全屏重驱（清灰染/残影）：back 置黑 → GC16 全屏 diff →
 * 白区全驱驱白（run98 白驱基线同构；库刷后 back 脏行同步=front）。
 * ghost-clear（U 命令）与 K 重置共用；不依赖内容变化 */
void epd_gfx_force_refresh(void)
{
    EpdiyHighlevelState *hl = panel_es108fc_hl();
    if (!hl || !s_canvas) return;

    uint8_t *fb = epd_hl_get_framebuffer(hl);
    const uint32_t *ex = s_expand;
    uint32_t *dst = (uint32_t *)fb;
    const uint8_t *src = s_canvas;
    for (size_t i = 0; i < CANVAS_BYTES; i++) dst[i] = ex[src[i]];

    const size_t fb_bytes = (size_t)(GFX_W / 2) * GFX_H;
    memset(hl->back_fb, 0x00, fb_bytes);
    int64_t t0 = esp_timer_get_time();
    enum EpdDrawError err = full_update_gc16(hl);
    int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;
    if (err != EPD_DRAW_SUCCESS) {
        ESP_LOGE(TAG, "强制重驱失败 err=%d（%s，%lld ms）", (int)err,
                 s_binfast ? "binfast" : "GC16", (long long)dt_ms);
    } else {
        ESP_LOGI(TAG, "灰染重置完成（%s，%lld ms）",
                 s_binfast ? "binfast" : "GC16", (long long)dt_ms);
    }
}

void epd_gfx_flush_window_passes(int x, int y, int w, int h, int passes)
{
    (void)passes;
    epd_gfx_flush_window(x, y, w, h);
}

/* ---- binfast 自驱深清（run115 序列移植，2026-09-17）----
 * 白 → 黑 → 白 → 画布内容，四段全驱。纯色段一律 binfast 等幅方波
 * （每扫满摆幅、无相位依赖）：GC16 短相位会被 LCD 排水不均的扫描行
 * 跳过 → 黑→白未完成横向灰纹（LAN 固件五轮真机实证，见
 * docs/BIGSCREEN_IMAGE_QUALITY_ANALYSIS.md §11.2）。与 s_binfast 开关
 * 无关（深清的目的就是满摆幅，GC16 段反而引入灰纹）。约 4×0.7s。
 * 画布为内容权威源，末段重展开恢复，无需备份 fb。 */
void epd_gfx_deep_clean(void)
{
    EpdiyHighlevelState *hl = panel_es108fc_hl();
    if (!hl || !s_canvas) return;
    uint8_t *fb = epd_hl_get_framebuffer(hl);
    const size_t fb_bytes = (size_t)(GFX_W / 2) * GFX_H;
    const EpdWaveform *orig = hl->waveform;
    hl->waveform = binfast_waveform();
    int64_t t0 = esp_timer_get_time();
    btn_guard_begin();
    enum EpdDrawError err = EPD_DRAW_SUCCESS;
    const struct { uint8_t front, back; } seq[3] = {
        {0xFF, 0x00}, {0x00, 0xFF}, {0xFF, 0x00},
    };
    for (int k = 0; k < 3 && err == EPD_DRAW_SUCCESS; k++) {
        memset(fb, seq[k].front, fb_bytes);
        memset(hl->back_fb, seq[k].back, fb_bytes);
        err = epd_hl_update_screen(hl, MODE_GC16, epd_ambient_temperature());
    }
    if (err == EPD_DRAW_SUCCESS) {
        const uint32_t *ex = s_expand;
        uint32_t *dst = (uint32_t *)fb;
        const uint8_t *src = s_canvas;
        for (size_t i = 0; i < CANVAS_BYTES; i++) dst[i] = ex[src[i]];
        memset(hl->back_fb, 0xFF, fb_bytes);
        err = epd_hl_update_screen(hl, MODE_GC16, epd_ambient_temperature());
    }
    btn_guard_end();
    hl->waveform = orig;
    int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;
    if (err != EPD_DRAW_SUCCESS) {
        ESP_LOGE(TAG, "深清失败 err=%d（%lld ms）", (int)err, (long long)dt_ms);
    } else {
        ESP_LOGI(TAG, "深清完成（binfast 白黑白图，%lld ms）", (long long)dt_ms);
    }
}

bool epd_gfx_partial_supported(void) { return true; }
