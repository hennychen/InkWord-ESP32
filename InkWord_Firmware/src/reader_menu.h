/**
 * @file reader_menu.h
 * @brief 阅读器菜单覆盖层（2026-09-05 阅读器增强阶段六）
 *
 * 阅读模式下中键短按进入，提供以下菜单项：
 *   1. 我的书架   → book_shelf 覆盖层
 *   2. 目录       → 章节列表（内嵌子视图）
 *   3. 书签       → 书签列表（内嵌子视图）
 *   4. 搜索       → 搜索（内嵌子视图）
 *   5. 查看生词   → 生词列表（内嵌子视图）
 *   6. 阅读设置   → 字号调节
 *   7. 阅读统计   → 阅读时长/进度
 *
 * 子视图在 reader_menu 内部状态机切换（不额外 push 覆盖层），
 * RST 任意层级退出回阅读页。
 */
#ifndef INKWORD_READER_MENU_H
#define INKWORD_READER_MENU_H

#include "page_router.h"

#ifdef __cplusplus
extern "C" {
#endif

/** T1.4 页面协议实例（覆盖层入栈） */
extern const page_t g_reader_menu_page;

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_READER_MENU_H */
