/**
 * @file layout_profile.c
 * @brief L4 布局档位层实现（PANEL_COMPAT_DESIGN §8.1；Phase 5 交付物）
 */
#include "layout_profile.h"
#include "epd_driver.h"

#include <stdbool.h>

/* 档位参数表（§8.1 / §11.2；MID 列值 = 现役 416x240 视觉基线，
 * Phase 5 接线以现状值为准——设计文档 §8.1 示例 MID=2(24px) 指
 * 大字场景；阅读正文现状默认 20px，按视觉零变化铁律取 1）。
 *
 * T1.5 几何字段 = 迁移前各文件三元宏取值**原样搬运**：
 *   status_h/margin_x ← UI_STATUS_H/UI_MARGIN_X/MU_TITLE_H/MU_MARGIN_X
 *   body_reserve ← UI_BODY_RESERVE；item_h ← MU_ITEM_H/RV_ITEM_H
 *   hint_h ← MU_HINT_H（0/22）；rv_hint_h ← RV_HINT_H（18/24，与
 *   hint_h 值域不同故分列）；font_lvl_main/font_px_main/
 *   ascii_size_main/info_lh ← MU_FONT 系列/MU_INFO_LH
 *   tight_quote ← standby_page s_tight（kind<=SMALL 置位，含 TINY）
 * LARGE 档几何字段为预估值，「7.5" 上机校准」。
 * 字段初始化顺序与 struct 声明一致（kind, quote, reader, status_h,
 * margin_x, body_reserve, item_h, hint_h, rv_hint_h, font_lvl_main,
 * font_px_main, ascii_size_main, info_lh, tight_quote, narrow_tiny,
 * kb_scale, partial_std, partial_standby, partial_wifi）*/
static const layout_profile_t k_profiles[] = {
    [LAYOUT_TINY]  = { LAYOUT_TINY, 0, 0,
                       24,  8, 26, 28,  0, 18, 0, 16, 1, 20, 1, 0, 100,
                       100, 150, 125 },
                     /* 2.13"/2.9" 标签屏竖屏（2026-08-23 新增）：
                      * 引文/正文均 16px——短边 122~128px 下 24px 引文
                      * 8 字行宽 192px、正文 20px 每行仅 4~5 字，均不
                      * 可行；narrow_tiny 运行期按 122/128 宽覆盖 */
    [LAYOUT_SMALL] = { LAYOUT_SMALL, 2, 1,
                       32, 16, 30, 36, 22, 24, 1, 20, 2, 28, 1, 0, 60,
                       100, 150, 125 },
                     /* 2.7"：引文 24px（待机页 SMALL 紧排版配合，见
                      * tight_quote 字段）/ 正文 20px。2026-08-22 真机
                      * 勘误：初版全 16px 字小笔画糊（16px/117PPI≈
                      * 3.5mm，低于 3.7" 基线现感），升 24px(5.2mm)/
                      * 20px(4.3mm) 后改善 */
    [LAYOUT_MID]   = { LAYOUT_MID,   2, 1,
                       32, 16, 30, 44, 22, 24, 1, 20, 2, 28, 0, 0, 100,
                       100, 150, 125 },
                     /* 引文 24px / 正文 20px（现状） */
    [LAYOUT_LARGE] = { LAYOUT_LARGE, 3, 3,
                       32, 16, 30, 44, 22, 24, 1, 20, 2, 28, 0, 0, 100,
                       100, 150, 125 },
                     /* 引文 32px / 正文 32px（2026-09-03 32px 字库级
                      * 落地后升 3：7.5" 800x480 @~150PPI 下 24px 物理
                      * 字高仅 ~4mm 偏小，32px≈5.4mm 对齐 3.7" 基线
                      * 观感）；几何字段仍为预估，「7.5" 上机校准」 */
};

/* 档位缓存：首调填充；文件级供 test_reset 清（原 get 内 static 上提） */
static layout_profile_t s_prof;
static bool s_ready = false;

const layout_profile_t *layout_profile_get(void)
{
    if (!s_ready) {
        int w = epd_gfx_width(), h = epd_gfx_height();
        int short_px = (w < h) ? w : h;
        layout_kind_t k = (short_px < 140) ? LAYOUT_TINY
                        : (short_px < 200) ? LAYOUT_SMALL
                        : (short_px < 320) ? LAYOUT_MID
                                           : LAYOUT_LARGE;
        s_prof = k_profiles[k];
        /* TINY 档内窄屏特判档位化（原 `epd_gfx_width() <= 122` 宏
         * 判断收敛于此，T1.5）：OPM021EB 2.13" 122 宽（135DPI）辅助
         * 字级跟随正文；WFT0290 2.9" 128 宽（90DPI）保持 16px */
        s_prof.narrow_tiny = (k == LAYOUT_TINY && w <= 122) ? 1 : 0;
        s_ready = true;
    }
    return &s_prof;
}

void layout_profile_test_reset(void) { /* native-test 专用：清缓存 */
    s_ready = false;
}
