/**
 * @file reader_menu.h
 * @brief 大屏阅读菜单（覆盖层页；阅读页中键短按进入）
 *
 * 小屏 reader_menu 裁剪移植：主菜单 4 项（书架/目录/书签/阅读
 * 设置），内嵌子视图状态机（不额外 push 覆盖层）；搜索/生词联动/
 * 阅读统计后置。书架 = 内置演示书 + SPIFFS 书库（storage_books_list），
 * 小屏为独立 book_shelf 页，大屏收敛为子视图。
 *
 * 按键：UP/DOWN 选择，CENTER 确认/跳转/步进，SET 主菜单 = 当前页
 * 书签切换 / 书签列表 = 删除选中，RST 逐级返回（主菜单层 = 退出）。
 * 跳转/选书/设置步进后 return false 退出菜单——页游标单点持有于
 * reader_page（goto/font_step/spacing_step/load_book 协作接口），
 * pop 后 render_top 统一恢复渲染。
 */
#ifndef BIGSCREEN_READER_MENU_H
#define BIGSCREEN_READER_MENU_H

#include "page_router.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 阅读菜单页（reader_page 中键经 page_router_push 入栈其上） */
extern const page_t g_reader_menu_page;

#ifdef __cplusplus
}
#endif
#endif /* BIGSCREEN_READER_MENU_H */
