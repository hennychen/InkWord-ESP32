/**
 * @file reader_page.h
 * @brief 大屏阅读页（覆盖层页方案，菜单「电子书」项入口）
 *
 * 栈串联制：menu_ui 入口 push 本页，阅读菜单（reader_menu）入栈
 * 其上，退出 = pop 逐层恢复。页游标单点持有于本模块（模式机
 * 零改动，MODE_READER 值位继续保留）。
 *
 * 页面结构（XLARGE 1920×1080）：
 *   状态栏 120px：书名 · 当前章节（cjk 24px）｜页码 N/M（FreeSans
 *                18pt；当前页有书签加 * 前缀）
 *   正文区：reader_engine 渲染（边距/行距/段距/分页见其文件头）
 *   提示栏 60px：按键提示（FreeSans 12pt）
 *
 * 按键（短按）：LEFT/UP=上一页  RIGHT/DOWN=下一页  CENTER=阅读菜单
 *               SET=书签添加/移除（切换）  RST=回首页
 * 按键（长按）：CENTER/RST=退出回菜单  SET=字号放大步进（循环）
 */
#ifndef BIGSCREEN_READER_PAGE_H
#define BIGSCREEN_READER_PAGE_H

#include "button_handler.h"
#include "page_router.h"

#ifdef __cplusplus
extern "C" {
#endif

/** T1.4 页面协议实例（menu「电子书」项经 page_router_push 入栈） */
extern const page_t g_reader_page;

/* ---- reader_menu 协作接口（页游标单点持有于本模块） ---- */

/** 当前页码（菜单目录定位/书签列表高亮用；未就绪 -1） */
int reader_page_current(void);

/** 页码跳转（只改游标不渲染；菜单 return false 后 render_top 统一恢复） */
void reader_page_goto(int page);

/** 字号步进（dir=±1 循环 20/24/32px；保位 + NVS 持久化） */
void reader_page_font_step(int dir);

/** 行距步进（dir=±1 循环 1.5/1.6/1.8 倍；保位 + NVS 持久化） */
void reader_page_spacing_step(int dir);

/**
 * @brief 加载书（书架选书后跳转用）。
 * @param path 书籍绝对路径；空串 = 内置演示书。
 *            游标取该书 NVS 进度（无记录回首页）。
 * @return 0 成功；-1 失败（保持原书，菜单可留在书架）。
 */
int reader_page_load_book(const char *path);

#ifdef __cplusplus
}
#endif
#endif /* BIGSCREEN_READER_PAGE_H */
