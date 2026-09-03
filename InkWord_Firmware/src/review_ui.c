/**
 * @file review_ui.c
 * @brief 复习模式词表视图（v1.3 PRD 5.2 / T1.3 自 main.cpp 迁出，修 A1）
 *
 * 迁移口径（T1.3，2026）：ui_draw_review_list 函数体零改动（导出名
 * review_ui_render_list）；RV_* 布局宏与布局派生宏自 main.cpp 同步
 * 复制（值与派生式逐字节一致；T1.5 参数表落地时一并收敛）；
 * s_review_detail/s_rv_off 状态原属 main.cpp 直读，迁出后经
 * is/set/reset 接口访问。
 */
#include "review_ui.h"

#include "debug_log.h"
#include "epd_driver.h"           /* epd_gfx_* */
#include "cjk_text.h"             /* cjk_text_draw / draw_wrap / width */
#include "layout_profile.h"       /* layout_profile_get / LAYOUT_SMALL */
#include "settings_ui.h"          /* settings_font_mode（UI_MEAN_LEVEL） */
#include "study_mode_machine.h"   /* study_mode_seq_total / seq_pos */
#include "learning_state.h"       /* learning_state_due_at */
#include "word_parser.h"          /* word_parser_get */
#include "haptic.h"               /* T2.2 路由迁入：haptic_event */
#include "ui_sfx.h"               /* T2.2 路由迁入：ui_sfx_play */
#include "page_router.h"          /* T2.2 渲染收敛：render_top */

/* ---- 布局宏（T1.5 参数表收敛完成：布局值查 layout_profile 字段，
 * 字号/行距派生式局部保留；语义同 main.cpp 学习页布局宏区）---- */
#define UI_TINY         (layout_profile_get()->kind == LAYOUT_TINY)
#define UI_STATUS_H     (layout_profile_get()->status_h)  /* 状态栏高度（T1.5） */
#define UI_MARGIN_X     (layout_profile_get()->margin_x)   /* 左右留白（T1.5） */
#define UI_MEAN_LEVEL   (layout_profile_get()->kind <= LAYOUT_SMALL \
                         ? (settings_font_mode() >= 1 ? 1 \
                            : (layout_profile_get()->narrow_tiny ? 1 : 0)) \
                         : (settings_font_mode() >= 1 ? 2 : 1))
#define UI_AUX_LEVEL    (layout_profile_get()->narrow_tiny ? UI_MEAN_LEVEL : 0)  /* 辅助字级（T1.5 档位化） */
#define UI_FOOT_BASE    (epd_gfx_height() - 16)        /* 底部标签基线：底边距 16 */
#define UI_FOOT_TOP     (UI_FOOT_BASE - (UI_AUX_LEVEL ? 22 : 18)) /* tag 点阵顶 */

/* ---- 复习模式词表视图（2026-08-24，PRD 5.2「紧凑显示+SRS 到期词」落地；
 * 百词斩复习范式借鉴）：到期词紧凑两列词表 + 中键进词卡详情，
 * 左/右自评即出队（游标钳位），列表态/详情态两态由渲染层承载 ---- */
#define RV_ITEM_H   (layout_profile_get()->item_h)  /* 对齐 menu_ui 列表行高（T1.5） */
#define RV_LIST_TOP (UI_STATUS_H + 4)
#define RV_HINT_H   (layout_profile_get()->rv_hint_h)  /* 底部提示行预留（T1.5） */
#define RV_VISIBLE  ((epd_gfx_height() - UI_STATUS_H - RV_HINT_H - 4) / RV_ITEM_H)
#define RV_SB_W     4                               /* 滚动条宽（menu_ui 同款） */
static bool s_review_detail = false;   /* false=词表 / true=词卡详情 */
static int  s_rv_off = 0;              /* 词表滚动窗口偏移 */

/* 两态接口（原 main.cpp 直读 static 变量，T1.3 起经导出访问） */
bool review_ui_is_detail(void) { return s_review_detail; }
void review_ui_set_detail(bool on) { s_review_detail = on; }
void review_ui_reset_detail(void) { s_review_detail = false; }

/* 复习模式按键路由（T2.2 自 main.cpp on_button 迁入；调用上下文：
 * MODE_REVIEW 短按、无覆盖层栈）。列表态=到期词紧凑词表：上/下
 * 移动选择、中进词卡详情、左/右自评出队（游标钳位，对应底部提示
 * 行「中 详情 · 左/右 自评出队」）、RST 回首行、SET 无遮蔽语义
 * 忽略；空序列全忽略（空态页无交互对象，评分目标词索引无效）。
 * 详情态仅左/右自评出队并回列表（after_due_review；序列清空由
 * 列表态空态页承载，避免空序列词卡取词），其余返回 false 放行
 * 通用词卡路由（上/下翻释义页/跨词翻卡、中发音、SET 遮蔽、
 * RST 设置直达） */
bool review_ui_on_button(nav_key_t id, button_event_t event)
{
    (void)event;   /* 调用上下文已限定短按 */

    if (!s_review_detail) {
        switch (id) {
        case NAV_UP:
            study_mode_handle_action(0);   /* 选择上一词（回绕，内部重绘） */
            return true;
        case NAV_DOWN:
            study_mode_handle_action(1);   /* 选择下一词 */
            return true;
        case NAV_CENTER:
            if (study_mode_seq_total() == 0) return true;
            review_ui_set_detail(true);       /* 进词卡详情 */
            page_router_render_top();         /* base 分流：详情词卡 */
            return true;
        case NAV_LEFT:
        case NAV_RIGHT: {
            if (study_mode_seq_total() == 0) return true;
            int q = (id == NAV_RIGHT) ? 5 : 1;
            learning_state_apply_quality(
                study_mode_current_word_index(), q);
            haptic_event(HAPTIC_REVIEW);   /* 自评提交 30ms（PRD 5.4） */
            ui_sfx_play(UI_SFX_RATE);      /* T1.6 自评提交音「滴答」 */
            study_mode_after_due_review(); /* REVIEW 恒 true：出队钳位 */
            page_router_render_top();
            return true;
        }
        case NAV_RST:
            study_mode_reset_cursor();     /* 回首行 */
            return true;
        default:                           /* NAV_SET：列表态无遮蔽语义 */
            return true;
        }
    }

    /* 详情态自评：出队 + 回列表（下词钳位高亮；清空→空态页） */
    if (id == NAV_LEFT || id == NAV_RIGHT) {
        int q = (id == NAV_RIGHT) ? 5 : 1;
        learning_state_apply_quality(study_mode_current_word_index(), q);
        haptic_event(HAPTIC_REVIEW);
        ui_sfx_play(UI_SFX_RATE);      /* T1.6 自评提交音「滴答」 */
        review_ui_set_detail(false);
        study_mode_after_due_review();
        page_router_render_top();   /* 出队回列表（清空→空态页） */
        return true;
    }
    return false;   /* 其余键放行通用词卡路由 */
}

/* 复习到期词表（列表态）：两列紧凑行（左词 FreeSans / 右释义首行
 * 截断点阵）+ 反选高亮 + 滚动条 + 底部提示（menu_ui 列表范式）；
 * 行取词直接经 learning_state_due_at（REVIEW 序列=due 视图） */
void review_ui_render_list(void)
{
    int total = study_mode_seq_total();
    int sel   = study_mode_seq_pos();

    /* 滚动窗口跟随 */
    if (sel < s_rv_off) s_rv_off = sel;
    if (sel >= s_rv_off + RV_VISIBLE) s_rv_off = sel - RV_VISIBLE + 1;
    int max_off = total > RV_VISIBLE ? total - RV_VISIBLE : 0;
    if (s_rv_off > max_off) s_rv_off = max_off;
    if (s_rv_off < 0) s_rv_off = 0;

    epd_gfx_fill_rect(0, UI_STATUS_H, epd_gfx_width(),
                      epd_gfx_height() - UI_STATUS_H, EPD_GFX_WHITE);

    int body_w = epd_gfx_width() - 2 * UI_MARGIN_X - RV_SB_W - 4;
    for (int i = 0; i < RV_VISIBLE; i++) {
        int idx = s_rv_off + i;
        if (idx >= total) break;
        int wi = learning_state_due_at(idx);
        const WordEntry *w = wi >= 0 ? word_parser_get(wi) : NULL;
        if (!w) break;

        int y = RV_LIST_TOP + i * RV_ITEM_H;
        bool s = (idx == sel);
        if (s)
            epd_gfx_fill_rect(UI_MARGIN_X, y, body_w, RV_ITEM_H - 4,
                              EPD_GFX_BLACK);

        /* 左：词（FreeSans size 2；半宽 ASCII 与点阵释义行视觉平衡） */
        int tw, th;
        epd_gfx_text_bounds(w->text, 2, &tw, &th);
        epd_gfx_draw_text(UI_MARGIN_X + 4, y + RV_ITEM_H * 3 / 4,
                          w->text, s ? EPD_GFX_WHITE : EPD_GFX_BLACK, 2);

        /* 右：释义首行截断（16px 点阵单行；剩宽 <32px 跳过） */
        int mx = UI_MARGIN_X + 4 + tw + 12;
        int mw = UI_MARGIN_X + body_w - 6 - mx;
        if (mw >= 32 && w->meaning[0])
            cjk_text_draw_wrap(mx, y + (RV_ITEM_H - 16) / 2, mw, 0, 0, 1,
                               w->meaning, s ? EPD_GFX_WHITE : EPD_GFX_BLACK);
    }

    /* 滚动条（menu_ui 同款滑块） */
    if (total > RV_VISIBLE) {
        int x = epd_gfx_width() - UI_MARGIN_X;
        int h = RV_VISIBLE * RV_ITEM_H;
        epd_gfx_draw_rect(x, RV_LIST_TOP, RV_SB_W, h, EPD_GFX_BLACK);
        int thumb_h = h * RV_VISIBLE / total;
        if (thumb_h < RV_SB_W * 2) thumb_h = RV_SB_W * 2;
        epd_gfx_fill_rect(x, RV_LIST_TOP + (h - thumb_h) * s_rv_off /
                          (total - RV_VISIBLE), RV_SB_W, thumb_h, EPD_GFX_BLACK);
    }

    /* 底部提示行（同学习页 foot 位；RST 直达设置 2026-08-27） */
    cjk_text_draw(UI_MARGIN_X, UI_FOOT_TOP, 0,
                  "中 详情 · 左/右 自评出队 · RST 设置", EPD_GFX_BLACK);
}
