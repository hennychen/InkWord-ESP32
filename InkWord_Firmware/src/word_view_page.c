/**
 * @file word_view_page.c
 * @brief 词卡视图通用按键路由实现（架构拆分 2026-09-17，自 main.cpp 迁出）
 *
 * base 层（FLASH/听写/拼写）与学习视图栈页（错词本/收藏/墨封录）共用
 * 同源按键编排。行为零变化，纯搬移。
 */
#include "word_view_page.h"
#include "page_router.h"
#include "word_card_ui.h"
#include "haptic.h"
#include "ui_sfx.h"
#include "learning_state.h"
#include "ui_stamp.h"
#include "refresh_scheduler.h"
#include "debug_log.h"

static const char *TAG = "WORD_VIEW";

/* 跨模块页全局（定义在各功能模块；前向声明避免拉入重头文件） */
extern const page_t g_menu_ui_page;
extern const page_t g_portal_page;
extern const page_t g_lan_page;
extern const page_t g_wrongbook_page;
extern const page_t g_settings_ui_page;

bool word_view_on_button(study_mode_t m, nav_key_t id,
                         button_event_t event)
{
    if (event == BUTTON_EVENT_LONG_PRESS) {
        switch (id) {
        case NAV_CENTER:
            page_router_push(&g_menu_ui_page);   /* T1.4：enter=menu_ui_enter */
            return true;
        case NAV_UP:
            LOG_I("user requested ghost-clear full refresh");
            refresh_force_full();
            return true;
        case NAV_DOWN:
            /* 切换学习模式并重绘（ui_render_word 检测到模式变化自动全刷） */
            haptic_event(HAPTIC_MODE);   /* 模式切换 50ms（PRD 5.4） */
            ui_sfx_play(UI_SFX_MODE);    /* T1.6 模式切换音「滴--」 */
            study_mode_switch_next();
            page_router_render_top();
            return true;
        case NAV_LEFT:
            page_router_push(&g_portal_page);   /* 栈串联：portal 栈化 */
            return true;
        case NAV_RIGHT:
            page_router_push(&g_lan_page);      /* 栈串联：LAN 接收页栈化 */
            return true;
        case NAV_SET:
            /* 收藏/取消当前词（P1）：局部重绘内容区刷新 * 标记；
             * 阅读模式无"当前词"概念，不响应；
             * 收藏视图（MODE_COLLECTION）内=移出序列（after_uncollect
             * 收缩钳位，清空自动退视图），2026-08-23；
             * 墨封录（MODE_MASTERED）内=启封当前词移出序列
             * （after_master 同构，启封无动画，2026-09-04）；栈化后
             * 清空（after_* 内部归位 FLASH）以模式变化判据退视图 */
            if (m == MODE_READER) return true;
            haptic_event(HAPTIC_REVIEW); /* 确认型操作归自评档 30ms */
            if (m == MODE_MASTERED) {
                learning_state_toggle_master(study_mode_current_word_index());
                study_mode_after_master();
                if (study_mode_current() != MODE_MASTERED)
                    return false;   /* 清空：退视图回上级（栈页 pop） */
                page_router_render_top();   /* 游标收缩留视图重绘 */
                return true;
            }
            learning_state_toggle_collect(study_mode_current_word_index());
            if (m == MODE_COLLECTION) {
                study_mode_after_uncollect();
                if (study_mode_current() != MODE_COLLECTION)
                    return false;   /* 清空：退视图回上级 */
            }
            page_router_render_top();  /* 星标局部重绘/游标收缩 */
            return true;
        case NAV_RST:
            /* 临时视图=RST 长按退出（原四级判语义；dispatch 统一 pop
             * 回上级）；base=进错词本（无错词 100ms 长震+拒绝音，PRD 5.4） */
            if (m == MODE_WRONGBOOK || m == MODE_COLLECTION ||
                m == MODE_MASTERED)
                return false;
            if (!study_mode_enter_wrongbook()) {
                haptic_event(HAPTIC_ERROR);
                ui_sfx_play(UI_SFX_ERR); /* T1.6 边界拒绝音「嘟-」 */
                return true;
            }
            haptic_event(HAPTIC_MODE);
            page_router_push(&g_wrongbook_page);   /* 状态已置，enter=首帧 */
            return true;
        default:
            return true;
        }
    }
    if (event != BUTTON_EVENT_SHORT_PRESS) return true;

    /* 短按：上/下翻词（释义多页时先词内翻释义页），中=发音，
     * SET=遮蔽/揭晓释义，RST=直达设置页（2026-08-27 音量等高频项
     * 快速触达）；左=自评「忘记」Q1，右=自评「简单」Q5（FSRS 评分入
     * learning_state，错词本内答对自动移出，序列清空自动退视图） */
    switch (id) {
    case NAV_UP:
        if (ui_mean_page_step(-1)) return true;  /* 释义多页：词内上一页 */
        study_mode_handle_action(0);   /* prev */
        return true;
    case NAV_DOWN:
        if (ui_mean_page_step(+1)) return true;  /* 释义多页：词内下一页 */
        study_mode_handle_action(1);   /* next */
        return true;
    case NAV_CENTER:
        study_mode_handle_action(3);   /* speak */
        return true;
    case NAV_SET:
        study_mode_handle_action(2);   /* confirm：遮蔽/揭晓释义 */
        return true;
    case NAV_RST:
        page_router_push(&g_settings_ui_page);  /* T1.4：enter=settings_ui_enter */
        return true;
    case NAV_LEFT:
        learning_state_apply_quality(study_mode_current_word_index(), 1);
        haptic_event(HAPTIC_REVIEW);   /* 自评提交 30ms（PRD 5.4） */
        ui_sfx_play(UI_SFX_RATE);      /* T1.6 自评提交音「滴答」 */
        if (study_mode_after_quality(1))
            page_router_render_top();
        return true;
    case NAV_RIGHT:
        learning_state_apply_quality(study_mode_current_word_index(), 5);
        haptic_event(HAPTIC_REVIEW);   /* 自评提交 30ms（PRD 5.4） */
        ui_sfx_play(UI_SFX_RATE);      /* T1.6 自评提交音「滴答」 */
        /* 2026-09-04：自评简单联动墨封——用户认为简单=已掌握，
         * 置位方向播旋转盖章动画（toggle 幂等，已墨封词不重复触发） */
        {
            int wi_rt = study_mode_current_word_index();
            if (wi_rt >= 0) {
                bool just_mastered = learning_state_toggle_master(wi_rt);
                if (just_mastered) ui_stamp_play();
            }
        }
        /* 墨封/自评后序列收缩 + 清空自动退视图（原 base 层清空退闪卡
         * 由 render_top 自适应；栈化后模式归位=pop 栈页回上级） */
        study_mode_after_master();
        if ((m == MODE_WRONGBOOK || m == MODE_MASTERED) &&
            study_mode_current() != m)
            return false;   /* 清空：退视图回上级 */
        page_router_render_top();
        return true;
    default:
        return true;
    }
}
