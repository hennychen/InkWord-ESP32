/**
 * @file ui_stamp.c
 * @brief 墨封圆形印章 + 词卡「熟」角标实现（设计见 ui_stamp.h）
 *
 * 单帧圆形印章（同步阻塞 ~100ms）：
 *   双同心圆环（外 r=52 w=3 / 内 r=44 w=2）+ 断线纹理（压印质感）
 *   + 印泥纹理黑点（环间 4% 密度）+ 中心「熟」字。
 * 直接画在词卡上，调用方 after_master + render_top 翻页覆盖。
 *
 * 断线策略：Bresenham 逐像素画圆，按角度位置跳过——外环 10%、
 * 内环 6%，确定性伪随机（固定种子，每次印章外观一致）。
 * 印泥纹理：固定种子伪随机在环间区域散布 1px 黑点，密度 4%。
 */
#include "ui_stamp.h"
#include "epd_driver.h"
#include "epd_panel.h"      /* EPD_GFX_BLACK/WHITE 唯一权威定义 */
#include "cjk_text.h"
#include "layout_profile.h" /* status_h / LAYOUT_TINY（正文区几何） */
#include "haptic.h"
#include "ui_sfx.h"

#include <math.h>           /* atan2f */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ---- 确定性伪随机（固定种子，每次印章外观一致） ---- */
static unsigned s_rng;
static void rng_seed(unsigned s) { s_rng = s ? s : 1; }
static unsigned rng_next(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

/* ---- 断线圆环：Bresenham 逐像素画圆，按角度跳过 ----
 * cx,cy 圆心；r 半径；skip_pct 跳过百分比（0~100）；color 颜色。
 * 跳过判定：atan2(dy,dx) 映射到 [0,360)，每 30° 一档，
 * rng_next()%100 < skip_pct 则跳过该档全部像素。
 * 效果：圆弧被随机分成 ~12 段，部分段缺失 = 压印断线质感。 */
static void draw_ring_broken(int cx, int cy, int r,
                             int skip_pct, uint16_t color)
{
    int x = 0, y = r;
    int d = 3 - 2 * r;

    while (y >= x) {
        /* 八对称点 */
        int px[8] = { cx+x, cx-x, cx+x, cx-x, cx+y, cx-y, cx+y, cx-y };
        int py[8] = { cy+y, cy+y, cy-y, cy-y, cy+x, cy+x, cy-x, cy-x };
        for (int i = 0; i < 8; i++) {
            int ddx = px[i] - cx;
            int ddy = py[i] - cy;
            int ang = (int)(atan2f((float)ddy, (float)ddx) * 57.2958f);
            if (ang < 0) ang += 360;
            /* 每 30° 一档，rng 决定该档是否跳过 */
            int slot = ang / 30;
            /* 每个 slot 独立判定（用 slot+round 做种子） */
            unsigned saved = s_rng;
            s_rng = (unsigned)(slot * 7 + r * 13 + 1);
            bool skip = (int)(rng_next() % 100) < skip_pct;
            s_rng = saved;
            if (!skip)
                epd_gfx_fill_rect(px[i], py[i], 1, 1, color);
        }
        x++;
        if (d < 0) {
            d += 4 * x + 1;
        } else {
            y--;
            d += 4 * (x - y) + 1;
        }
    }
}

/* ---- 印章主体 ----
 * 按用户规格：
 *   外圆环 r=52 w=3（画 r=50,51,52 三层）断线 10%
 *   内圆环 r=44 w=2（画 r=43,44 两层）断线 6%
 *   环间印泥黑点 4% 密度（固定种子伪随机）
 *   中心「熟」字 24px（cjk font_size=2） */
static void draw_stamp(int cx, int cy)
{
    rng_seed(0x544D5021);   /* "TMP!" 固定种子 */

    /* 外圆环 r=52 w=3：画 r=50,51,52 三层 Bresenham 圆 */
    for (int dr = 0; dr < 3; dr++)
        draw_ring_broken(cx, cy, 50 + dr, 10, EPD_GFX_BLACK);

    /* 内圆环 r=44 w=2：画 r=43,44 两层 */
    for (int dr = 0; dr < 2; dr++)
        draw_ring_broken(cx, cy, 43 + dr, 6, EPD_GFX_BLACK);

    /* 印泥纹理：环间区域（r∈[46,49]）散布 1px 黑点，密度 4% */
    for (int i = 0; i < 500; i++) {
        int dx = (int)(rng_next() % 99) - 49;     /* [-49, 49] */
        int dy = (int)(rng_next() % 99) - 49;
        int dist2 = dx * dx + dy * dy;
        if (dist2 >= 46 * 46 && dist2 <= 49 * 49)
            epd_gfx_fill_rect(cx + dx, cy + dy, 1, 1, EPD_GFX_BLACK);
    }

    /* 中心「熟」字（24px cjk，居中于圆心） */
    cjk_text_draw(cx - 12, cy - 12, 2, "熟", EPD_GFX_BLACK);
}

void ui_stamp_play(void)
{
    int w = epd_gfx_width();
    int h = epd_gfx_height();
    int top = layout_profile_get()->status_h;   /* 正文区顶 */
    int cy = top + (h - top) / 2;               /* 正文区几何中心 */
    int cx = w / 2;

    /* 单帧直接画完整印章——盖在词卡上，不清屏 */
    draw_stamp(cx, cy);
    haptic_pulse(80);                           /* 重震一记（盖章手感） */
    ui_sfx_play(UI_SFX_STAMP);                  /* 「咚」（缺样本静默降级） */
    epd_gfx_flush_window(0, top, w, h - top);
    vTaskDelay(pdMS_TO_TICKS(100));             /* 100ms 视觉停留 */
}

void ui_draw_seal_mark(int right_x, int y)
{
    /* 20px 空心方框 + 16px「熟」居中（2px 边距，印章边框意象）；
     * y = 16px 行顶左基准，框上提 2px 垂直居中 */
    epd_gfx_draw_rect(right_x - 20, y - 2, 20, 20, EPD_GFX_BLACK);
    cjk_text_draw(right_x - 18, y, 0, "熟", EPD_GFX_BLACK);
}
