/**
 * @file review_ui.h
 * @brief 复习模式词表视图（v1.3 PRD 5.2 / T1.3 自 main.cpp 迁出，修 A1）
 *
 * 到期词紧凑两列词表 + 滚动条 + 底部提示（menu_ui 列表范式）；
 * 列表态/详情态两态——渲染入口由 ui_render_word 复习分支调用，
 * 两态切换经 is/set 接口由 on_button 复习路由驱动。
 */
#ifndef INKWORD_REVIEW_UI_H
#define INKWORD_REVIEW_UI_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 列表态渲染：滚动窗口跟随选择 + 两列行（左词/右释义截断）+
 *  反选高亮 + 滚动条 + 底部提示行 */
void review_ui_render_list(void);

/** 详情态查询（false=词表 / true=词卡详情；详情态只在会话内保持） */
bool review_ui_is_detail(void);

/** 详情态设置（on_button 复习路由：中键进详情 / 自评出队回列表） */
void review_ui_set_detail(bool on);

/** 模式切换重置列表态（ui_render_word：mode != s_last_mode 时） */
void review_ui_reset_detail(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_REVIEW_UI_H */
