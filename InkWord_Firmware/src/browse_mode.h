/**
 * @file browse_mode.h
 * @brief 教材目录浏览三级视图（2026-08-28，教材目录浏览+语音查词设计 §A2）
 *
 * 年级列表 → 单元列表 → 词表滚动三级页面状态机（数据源 catalog_index
 * 静态索引）。列表绘制复用 menu_ui 列表范式（反选高亮 + 滚动条 +
 * 页码；MU_* 同款 layout_profile 派生几何，TINY/SMALL/MID 全档零特判），
 * 在本模块内实现，不强行抽公共组件（最小变更）。
 *
 * 按键：上下=移动；中=进入下一级 / 词表页定位该词（study_mode_seek
 * 切闪卡）；RST 短按（或 SET）=返回上一级（词表→单元→年级→退出视图），
 * RST 长按=直接退出回闪卡（游标恢复进视图前位置）。
 *
 * 生命周期（T1.4 页面路由试点）：g_browse_page 经 page_router_push
 * 入栈（enter=reset 清态，首帧 render_top 走栈顶 render）；按键经
 * 栈顶 on_button 分发；退出/选词 seek 终结时 pop_if 归位。
 */
#ifndef INKWORD_BROWSE_MODE_H
#define INKWORD_BROWSE_MODE_H

#include "button_handler.h"
#include <stdbool.h>
#include "page_router.h"  /* T1.4：page_t（g_browse_page 导出） */

#ifdef __cplusplus
extern "C" {
#endif

/** 三级页面 */
typedef enum {
    BROWSE_GRADE = 0,   /**< 年级列表 */
    BROWSE_UNIT,        /**< 当前年级的单元列表 */
    BROWSE_WORDS,       /**< 当前单元的词表滚动 */
} browse_page_t;

/** 进入视图时清态（study_mode_enter_browse 成功后由菜单 act 调用） */
void browse_mode_reset(void);

/** 当前页全刷重绘（T1.4：render_top 的栈顶分发入口） */
void browse_mode_render(void);

/** 按键处理（T1.4：页面路由栈顶分发；长短按区分） */
void browse_mode_on_button(nav_key_t id, button_event_t event);

/** T1.4 页面协议实例（enter=reset；经 page_router_push 入栈，试点） */
extern const page_t g_browse_page;

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_BROWSE_MODE_H */
