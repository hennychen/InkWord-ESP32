/**
 * @file ui_stamp.c
 * @brief 墨封落印动画 + 词卡「熟」角标实现（设计见 ui_stamp.h）
 *
 * 旋转盖章动画（4 帧节拍式，同步阻塞 + vTaskDelay）：
 *   ①小方印胚（16px 实心）→ ②菱形旋转 45°（36px 空心）→
 *   ③大方形旋转回 0°（56px 空心）→ ④终印落地（80px 实心「熟」）→
 *   清白交调用方 render_top。
 * 方/菱交替 = 视觉旋转，尺寸递增 = 从小到大盖章。残影处理同
 * 前版：无窗口差分局刷自身无残影，末帧清白 + 调用方 render_top
 * 重绘收敛。
 * 「熟」字渲染上限 24px（cjk 字库 level 2），终印 TINY 56px / 常规 80px。
 */
#include "ui_stamp.h"
#include "epd_driver.h"
#include "epd_panel.h"      /* EPD_GFX_BLACK/WHITE 唯一权威定义 */
#include "cjk_text.h"
#include "layout_profile.h" /* status_h / LAYOUT_TINY（正文区几何） */
#include "haptic.h"
#include "ui_sfx.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define STAMP_BEAT_MS  250   /* 帧间停顿（盖章节奏，比前版 400ms 更紧凑） */

/* 实心方印「熟」：黑底 rect + 24px 白字居中 */
static void draw_seal(int cx, int cy, int size)
{
    int x0 = cx - size / 2;
    int y0 = cy - size / 2;
    epd_gfx_fill_rect(x0, y0, size, size, EPD_GFX_BLACK);
    cjk_text_draw(x0 + (size - 24) / 2, y0 + (size - 24) / 2,
                  2, "熟", EPD_GFX_WHITE);
}

/* 空心菱形（45° 旋转方形）：四顶点 (cx±r,cy)/(cx,cy±r)，斜率 ±1
 * epd_gfx 无 draw_line，逐像素 fill_rect 画四条边 */
static void draw_diamond(int cx, int cy, int r)
{
    for (int i = 0; i < r; i++) {
        epd_gfx_fill_rect(cx + r - i, cy + i, 1, 1, EPD_GFX_BLACK);   /* 顶→右 */
        epd_gfx_fill_rect(cx + r - i, cy + i, 1, 1, EPD_GFX_BLACK);   /* 右→底 */
        epd_gfx_fill_rect(cx - r + i, cy + r - i, 1, 1, EPD_GFX_BLACK); /* 底→左 */
        epd_gfx_fill_rect(cx - r + i, cy - i, 1, 1, EPD_GFX_BLACK);   /* 左→顶 */
    }
}

void ui_stamp_play(void)
{
    int w = epd_gfx_width();
    int h = epd_gfx_height();
    int top = layout_profile_get()->status_h;   /* 正文区顶（UI_STATUS_H 同源） */
    int cy = top + (h - top) / 2;               /* 正文区几何中心 */
    int cx = w / 2;
    int final_size = layout_profile_get()->kind == LAYOUT_TINY ? 56 : 80;

    /* 帧①小方印胚（16px 实心）——「印胚初现」 */
    epd_gfx_fill_rect(cx - 8, cy - 8, 16, 16, EPD_GFX_BLACK);
    epd_gfx_flush_window(0, top, w, h - top);
    haptic_pulse(30);
    vTaskDelay(pdMS_TO_TICKS(STAMP_BEAT_MS));

    /* 帧②菱形旋转 45°（36px 空心）——「旋转」 */
    epd_gfx_fill_rect(0, top, w, h - top, EPD_GFX_WHITE);
    draw_diamond(cx, cy, 18);
    epd_gfx_flush_window(0, top, w, h - top);
    vTaskDelay(pdMS_TO_TICKS(STAMP_BEAT_MS));

    /* 帧③大方形旋转回 0°（56px 空心）——「旋转回正」 */
    epd_gfx_fill_rect(0, top, w, h - top, EPD_GFX_WHITE);
    epd_gfx_draw_rect(cx - 28, cy - 28, 56, 56, EPD_GFX_BLACK);
    epd_gfx_flush_window(0, top, w, h - top);
    vTaskDelay(pdMS_TO_TICKS(STAMP_BEAT_MS));

    /* 帧④终印落地（final_size 实心「熟」）——「盖章」 */
    epd_gfx_fill_rect(0, top, w, h - top, EPD_GFX_WHITE);
    draw_seal(cx, cy, final_size);
    haptic_pulse(80);                           /* 重震一记（盖章手感） */
    ui_sfx_play(UI_SFX_STAMP);                  /* 「咚」（缺样本静默降级） */
    epd_gfx_flush_window(0, top, w, h - top);
    vTaskDelay(pdMS_TO_TICKS(STAMP_BEAT_MS));

    /* 清白：词卡恢复由调用方 render_top 重绘（带角标/下词） */
    epd_gfx_fill_rect(0, top, w, h - top, EPD_GFX_WHITE);
    epd_gfx_flush_window(0, top, w, h - top);
}

void ui_draw_seal_mark(int right_x, int y)
{
    /* 20px 空心方框 + 16px「熟」居中（2px 边距，印章边框意象）；
     * y = 16px 行顶左基准，框上提 2px 垂直居中 */
    epd_gfx_draw_rect(right_x - 20, y - 2, 20, 20, EPD_GFX_BLACK);
    cjk_text_draw(right_x - 18, y, 0, "熟", EPD_GFX_BLACK);
}
