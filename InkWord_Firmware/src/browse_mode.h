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
 * 生命周期对齐临时视图第五先例：browse_mode_reset 由菜单 act 在
 * study_mode_enter_browse 成功后调用；渲染经 ui_render_current 的
 * MODE_BROWSE 分流（首帧全刷）；按键经 main.cpp on_button 转发
 * （长短按都要，RST 长按语义在键值区分）。
 */
#ifndef INKWORD_BROWSE_MODE_H
#define INKWORD_BROWSE_MODE_H

#include "button_handler.h"
#include <stdbool.h>

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

/** 当前页全刷重绘（ui_render_current 的 MODE_BROWSE 分流入口） */
void browse_mode_render(void);

/** 按键处理（main.cpp on_button 的 MODE_BROWSE 转发；长短按区分） */
void browse_mode_on_button(nav_key_t id, button_event_t event);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_BROWSE_MODE_H */
