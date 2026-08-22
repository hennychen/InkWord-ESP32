/**
 * @file layout_profile.c
 * @brief L4 布局档位层实现（PANEL_COMPAT_DESIGN §8.1；Phase 5 交付物）
 */
#include "layout_profile.h"
#include "epd_driver.h"

#include <stdbool.h>

/* 档位参数表（§8.1 / §11.2；MID 列值 = 现役 416x240 视觉基线，
 * Phase 5 接线以现状值为准——设计文档 §8.1 示例 MID=2(24px) 指
 * 大字场景；阅读正文现状默认 20px，按视觉零变化铁律取 1） */
static const layout_profile_t k_profiles[] = {
    [LAYOUT_SMALL] = { LAYOUT_SMALL, 2, 1 },  /* 2.7"：引文 24px（待机页
                                  * SMALL 紧排版配合，见 standby_page.c
                                  * s_tight）/ 正文 20px。2026-08-22 真机
                                  * 勘误：初版全 16px 字小笔画糊（16px/
                                  * 117PPI≈3.5mm，低于 3.7" 基线现感），
                                  * 升 24px(5.2mm)/20px(4.3mm) 后改善 */
    [LAYOUT_MID]   = { LAYOUT_MID,   2, 1 },  /* 引文 24px / 正文 20px（现状） */
    [LAYOUT_LARGE] = { LAYOUT_LARGE, 2, 2 },  /* 32px 级生成后升 3（§11.2） */
};

const layout_profile_t *layout_profile_get(void)
{
    static layout_profile_t s_prof;
    static bool s_ready = false;
    if (!s_ready) {
        int w = epd_gfx_width(), h = epd_gfx_height();
        int short_px = (w < h) ? w : h;
        layout_kind_t k = (short_px < 200) ? LAYOUT_SMALL
                        : (short_px < 320) ? LAYOUT_MID
                                           : LAYOUT_LARGE;
        s_prof = k_profiles[k];
        s_ready = true;
    }
    return &s_prof;
}
