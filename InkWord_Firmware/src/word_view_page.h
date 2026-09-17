/**
 * @file word_view_page.h
 * @brief 词卡视图通用按键路由（架构拆分 2026-09-17，自 main.cpp 迁出）
 *
 * base 层（FLASH/听写/拼写）与学习视图栈页（错词本/收藏/墨封录）共用
 * 同源按键编排：上/下翻词（释义多页先词内翻页），中=发音，SET=遮蔽/揭晓，
 * RST=设置页；长按六键（菜单/清残影/模式切换/门户/LAN/收藏/错词本）。
 * 返回 false = 请求退出视图（栈页 dispatch 统一 pop+render_top）。
 */
#ifndef INKWORD_WORD_VIEW_PAGE_H
#define INKWORD_WORD_VIEW_PAGE_H

#include "study_mode_machine.h"   /* study_mode_t */
#include "button_handler.h"       /* nav_key_t / button_event_t */
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 词卡视图通用按键路由（base + 临时视图栈页共用）。
 * @param m 当前模式（base 层传 study_mode_current()；栈页传固定模式值）
 * @param id 按键
 * @param event 事件（短按/长按）
 * @return true=已消费；false=请求退出视图
 */
bool word_view_on_button(study_mode_t m, nav_key_t id,
                         button_event_t event);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_WORD_VIEW_PAGE_H */
