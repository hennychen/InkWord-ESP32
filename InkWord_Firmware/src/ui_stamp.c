/**
 * @file ui_stamp.c
 * @brief 墨封圆形盖章动画 + 词卡「熟」角标实现（设计见 ui_stamp.h）
 *
 * 圆形盖章动画（3 帧节拍式，同步阻塞 ~450ms）：
 *   ①小圆点（12px）→ ②中圆（36px）→ ③大圆印「熟」（80px）。
 * 尺寸递增 = 从小到大盖章，圆形 = 印章意象。末帧保留圆印，
 * 调用方 render_top 渲染新词直接覆盖（新词内容覆盖整个正文区，
 * 圆印像素被新词背景替代，无白屏过渡）。
 * 「熟」字渲染上限 24px（cjk 字库 level 2），终印 TINY 56px / 常规 80px。
 */
#include "ui_stamp.h"
#include "epd_driver.h"
#include "epd_panel.h"      /* EPD_GFX_BLACK/WHITE 唯一权威定义 */
#include "cjk_text.h"
#include "layout_profile.h" /* status_h / LAYOUT_TINY（正文区几何） */
#include "haptic.h"
#include "ui_sfx.h"

#include <math.h>          /* sqrtf（fill_circle 中点圆算法） */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define STAMP_BEAT_MS  150   /* 帧间停顿（3 帧 ~450ms 快速盖章） */

/* 实心圆（中点圆算法逐行填充）：epd_gfx 无 fill_circle，
 * 用 draw_hline 逐行画——每行宽度由圆方程 sqrt(r²-dy²) 决定 */
static void fill_circle(int cx, int cy, int r, uint16_t color)
{
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)(0.5f + sqrtf((float)(r * r - dy * dy)));
        epd_gfx_draw_hline(cx - dx, cy + dy, 2 * dx + 1, color);
    }
}

/* 圆形印「熟」：黑底圆 + 24px 白字居中 */
static void draw_circle_seal(int cx, int cy, int r)
{
    fill_circle(cx, cy, r, EPD_GFX_BLACK);
    /* 「熟」字居中（24px cjk，字面占圆面 ~30%） */
    cjk_text_draw(cx - 12, cy - 12, 2, "熟", EPD_GFX_WHITE);
}

void ui_stamp_play(void)
{
    int w = epd_gfx_width();
    int h = epd_gfx_height();
    int top = layout_profile_get()->status_h;   /* 正文区顶（UI_STATUS_H 同源） */
    int cy = top + (h - top) / 2;               /* 正文区几何中心 */
    int cx = w / 2;
    int final_r = layout_profile_get()->kind == LAYOUT_TINY ? 28 : 40;

    /* 帧①小圆点（12px）——「印胚初现」 */
    fill_circle(cx, cy, 6, EPD_GFX_BLACK);
    haptic_pulse(30);
    epd_gfx_flush_window(0, top, w, h - top);
    vTaskDelay(pdMS_TO_TICKS(STAMP_BEAT_MS));

    /* 帧②中圆（36px）——「盖下」 */
    epd_gfx_fill_rect(0, top, w, h - top, EPD_GFX_WHITE);
    fill_circle(cx, cy, 18, EPD_GFX_BLACK);
    epd_gfx_flush_window(0, top, w, h - top);
    vTaskDelay(pdMS_TO_TICKS(STAMP_BEAT_MS));

    /* 帧③大圆印「熟」（final_r）——「盖章落地」
     * 末帧保留圆印：调用方 render_top 渲染新词直接覆盖整个正文区，
     * 圆印黑色像素被新词背景替代，无白屏过渡 */
    epd_gfx_fill_rect(0, top, w, h - top, EPD_GFX_WHITE);
    draw_circle_seal(cx, cy, final_r);
    haptic_pulse(80);                           /* 重震一记（盖章手感） */
    ui_sfx_play(UI_SFX_STAMP);                  /* 「咚」（缺样本静默降级） */
    epd_gfx_flush_window(0, top, w, h - top);
}

void ui_draw_seal_mark(int right_x, int y)
{
    /* 20px 空心方框 + 16px「熟」居中（2px 边距，印章边框意象）；
     * y = 16px 行顶左基准，框上提 2px 垂直居中 */
    epd_gfx_draw_rect(right_x - 20, y - 2, 20, 20, EPD_GFX_BLACK);
    cjk_text_draw(right_x - 18, y, 0, "熟", EPD_GFX_BLACK);
}
