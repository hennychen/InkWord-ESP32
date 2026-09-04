/**
 * @file ui_stamp.c
 * @brief 墨封落印动画 + 词卡「熟」角标实现（设计见 ui_stamp.h）
 *
 * 节拍式分镜（quiz 反馈帧 QUIZ_FB_OK_MS 同上下文先例：同步阻塞 +
 * vTaskDelay 节拍，局刷窗口限正文区不碰状态栏）；残影：动画局刷
 * 直连 epd_gfx_flush_window，不经 refresh_scheduler 计数，但无窗口
 * 差分局刷自身无残影，幕三清白 + 调用方 render_top 重绘收敛，无需
 * 保养特判。
 * 「熟」字渲染上限 24px（cjk 字库 level 2），主印 56px（TINY 40px）
 * ——字面占印面 ~43%，黑白对比下清晰可读（印章密加盖风格）。
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

#define STAMP_BEAT_MS  400   /* 幕间停顿（quiz 反馈帧同档节拍） */

/* 幕数档位（运行期读面板 desc）：无局刷（三色屏）或慢局刷
 * （>700ms）1 幕；标准局刷（>400ms）2 幕；快屏 3 幕 */
static int stamp_beats(void)
{
    const epd_panel_desc_t *pd = epd_panel_desc();
    if (!pd || !pd->partial_enabled) return 1;
    if (pd->partial_ms > 700) return 1;
    if (pd->partial_ms > 400) return 2;
    return 3;
}

/* 反白方印「熟」：黑底 rect + 24px 白字居中 */
static void draw_seal(int cx, int cy, int size)
{
    int x0 = cx - size / 2;
    int y0 = cy - size / 2;
    epd_gfx_fill_rect(x0, y0, size, size, EPD_GFX_BLACK);
    cjk_text_draw(x0 + (size - 24) / 2, y0 + (size - 24) / 2,
                  2, "熟", EPD_GFX_WHITE);
}

void ui_stamp_play(void)
{
    int w = epd_gfx_width();
    int h = epd_gfx_height();
    int top = layout_profile_get()->status_h;   /* 正文区顶（UI_STATUS_H 同源） */
    int cy = top + (h - top) / 2;               /* 正文区几何中心 */
    int cx = w / 2 + w / 8;                     /* 中央偏右（视觉重心） */
    int size = layout_profile_get()->kind == LAYOUT_TINY ? 40 : 56;

    /* 幕①：大印落下 */
    draw_seal(cx, cy, size);
    haptic_pulse(50);                           /* 重震一记（落印手感） */
    ui_sfx_play(UI_SFX_STAMP);                  /* 「咚」（缺样本静默降级） */
    epd_gfx_flush_window(0, top, w, h - top);
    vTaskDelay(pdMS_TO_TICKS(STAMP_BEAT_MS));

    /* 幕②（仅快屏）：印面微收正（清印区重画小印，差分=边缘收缩） */
    if (stamp_beats() == 3) {
        epd_gfx_fill_rect(cx - size / 2 - 2, cy - size / 2 - 2,
                          size + 4, size + 4, EPD_GFX_WHITE);
        draw_seal(cx, cy, size - 6);
        epd_gfx_flush_window(0, top, w, h - top);
        vTaskDelay(pdMS_TO_TICKS(STAMP_BEAT_MS));
    }

    /* 幕③：收印清白（词卡恢复由调用方 render_top 重绘带角标） */
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
