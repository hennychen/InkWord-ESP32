/**
 * @file layout_profile.c
 * @brief L4 布局档位层实现（PANEL_COMPAT_DESIGN §8.1；Phase 5 交付物）
 */
#include "layout_profile.h"
#include "epd_driver.h"
#include "cjk_font.h"   /* cjk_glyph_cell_size：字库级→cell px（P2 自动层派生） */

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
 * 字段初始化顺序与 struct 声明一致（kind, form, quote, reader, mean,
 * status_h, margin_x, body_reserve, item_h, hint_h, rv_hint_h,
 * font_lvl_main, font_px_main, ascii_size_main, info_lh, tight_quote,
 * narrow_tiny, kb_scale, partial_std, partial_standby, partial_wifi,
 * partial_menu）
 * form 表值恒 0（LANDSCAPE，get() 运行期覆盖，非表驱动） */
static const layout_profile_t k_profiles[] = {
    [LAYOUT_TINY]  = { LAYOUT_TINY, LAYOUT_FORM_LANDSCAPE, 0, 0, 0,
                       24,  8, 26, 28,  0, 18, 0, 16, 1, 20, 1, 0, 100,
                       100, 150, 125, 400 },
                     /* 2.13"/2.9" 标签屏竖屏（2026-08-23 新增）：
                      * 引文/正文均 16px——短边 122~128px 下 24px 引文
                      * 8 字行宽 192px、正文 20px 每行仅 4~5 字，均不
                      * 可行；narrow_tiny 运行期按 122/128 宽覆盖 */
    [LAYOUT_SMALL] = { LAYOUT_SMALL, LAYOUT_FORM_LANDSCAPE, 2, 1, 0,
                       32, 16, 30, 36, 22, 24, 1, 20, 2, 28, 1, 0, 60,
                       100, 150, 125, 400 },
                     /* 2.7"：引文 24px（待机页 SMALL 紧排版配合，见
                      * tight_quote 字段）/ 正文 20px / 释义 16px。2026-08-22
                      * 真机勘误：初版全 16px 字小笔画糊（16px/117PPI≈
                      * 3.5mm，低于 3.7" 基线现感），升 24px(5.2mm)/
                      * 20px(4.3mm) 后改善 */
    [LAYOUT_MID]   = { LAYOUT_MID,   LAYOUT_FORM_LANDSCAPE, 2, 1, 1,
                       32, 16, 30, 44, 22, 24, 1, 20, 2, 28, 0, 0, 100,
                       100, 150, 125, 400 },
                     /* 引文 24px / 正文 20px / 释义 20px（现状） */
    [LAYOUT_LARGE] = { LAYOUT_LARGE, LAYOUT_FORM_LANDSCAPE, 3, 3, 3,
                       32, 16, 30, 52, 32, 32, 3, 32, 3, 40, 0, 0, 100,
                       100, 150, 125, 400 },
                     /* 4.26" GDEQ0426T82（800x480 @219PPI）基线：主内容/
                      * 释义/引文均 32px（3.7mm）。表值 = PPI 自动层
                      * dpi=219 的推导结果（零变化验证）；7.5" 同分辨率
                      * @150PPI 经自动层降 24px（4.1mm）免校准 */
};

/* 档位缓存：首调填充；文件级供 test_reset 清（原 get 内 static 上提） */
static layout_profile_t s_prof;
static bool s_ready = false;
static uint16_t s_dpi = 0;   /* PPI 自动层输入（epd_driver_init 注入；0=表值） */

void layout_profile_set_dpi(uint16_t dpi)
{
    s_dpi = dpi;
}

/* PPI 自动层（2026-09-08）：物理字高目标（mm）+ 行宽容量（每行
 * min_chars 个全角字）双约束选最近字库级（16/20/24/32px 四级离散）；
 * dpi=0 返回 -1 退表值。复现验证（现役四档真机校准值零变化）：
 *   主内容 3.7mm：TINY@130 宽约束→0 / SMALL@117→1 / MID@130→1 /
 *   LARGE@219→3；释义 3.4mm：0/0/1/3（SMALL 词卡释义 16px 信息密度
 *   场景反推）；7.5" 800x480@150PPI 外推：主内容 22px→2（4.1mm） */
static int level_for_mm(float mm, int gfx_w, int min_chars)
{
    if (s_dpi == 0) return -1;
    int px = (int)(mm * s_dpi / 25.4f + 0.5f);
    int cap = gfx_w / min_chars;   /* 行宽容量：列表/正文行沿 gfx 宽 */
    if (px > cap) px = cap;
    if (px <= 16) return 0;
    if (px <= 20) return 1;
    if (px <= 24) return 2;
    return 3;
}

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
        /* 形态轴运行期填充（P2）：档位内横竖共存判据（MID 档 3.1"
         * 竖屏 vs 4.2"/3.7" 横屏），消费方读 form 勿再手写 h>w */
        s_prof.form = (w > h) ? LAYOUT_FORM_LANDSCAPE
                    : (w < h) ? LAYOUT_FORM_PORTRAIT
                              : LAYOUT_FORM_SQUARE;
        /* TINY 档内窄屏特判档位化（原 `epd_gfx_width() <= 122` 宏
         * 判断收敛于此，T1.5）：OPM021EB 2.13" 122 宽（135DPI）辅助
         * 字级跟随正文；WFT0290 2.9" 128 宽（90DPI）保持 16px */
        s_prof.narrow_tiny = (k == LAYOUT_TINY && w <= 122) ? 1 : 0;
        /* PPI 自动层：主内容/释义选级 + 几何公式化派生（覆盖表值；
         * 字号目标与复现验证见 level_for_mm 注）。派生口径：
         * item_h = cell + 档位 padding（12/16/24/20——四档现状值
         * 反推）；hint 类 = hint 级(主级-1) cell + 8（hint_h 16px
         * 级特殊取 22 保 MID/SMALL 零变化）；info_lh = cell + 8；
         * ascii 徽标 pt = 级<3 ? 级+1 : 3。TINY 提示栏省略 + 紧凑
         * INFO 不覆盖（表值口径） */
        int lvl = level_for_mm(3.7f, w, 8);
        if (lvl >= 0) {
            int cell = cjk_glyph_cell_size(lvl);
            static const int item_pad[] = {12, 16, 24, 20};
            s_prof.font_lvl_main = lvl;
            s_prof.font_px_main = cell;
            s_prof.ascii_size_main = lvl < 3 ? lvl + 1 : 3;
            s_prof.item_h = cell + item_pad[k];
            int mean = level_for_mm(3.4f, w, 8);
            if (mean >= 0) s_prof.mean_level = mean;
            if (k != LAYOUT_TINY) {
                int h_lvl = lvl > 0 ? lvl - 1 : 0;
                int h_cell = cjk_glyph_cell_size(h_lvl);
                s_prof.hint_h = h_lvl ? h_cell + 8 : 22;
                s_prof.rv_hint_h = h_cell + 8;
                s_prof.info_lh = cell + 8;
            }
        }
        s_ready = true;
    }
    return &s_prof;
}

void layout_profile_test_reset(void) { /* native-test 专用：清缓存
    *（dpi 不清——用例可自行 set_dpi 覆盖） */
    s_ready = false;
}
