/**
 * @file word_card_ui.h
 * @brief 大屏词卡页 —— 学习闭环主 UI（1920x1080 XLARGE 布局）
 *
 * 词典双栏（UI 重设计 2026-09-17）：顶栏黑底白字（模式名 + 收藏★/
 * 墨封徽标 + 序号）/ 左栏黑底（词头 48pt Bold 自适应降档 48→36→24pt
 * + 音标 40px 点阵 + 徽标行 tag·grade·source）/ 右栏白底释义 48px
 * 点阵断行（遮蔽/揭晓，多页词内翻页）/ 底栏静态键位提示带。
 * 刷新三档：首帧/切模式 GC16 全刷；翻词/收藏全屏窗口 DU；同词
 * 揭晓/释义翻页右栏窗口 DU（几何常量见 word_card_ui.c 文件头）。
 */
#ifndef INKWORD_WORD_CARD_UI_H
#define INKWORD_WORD_CARD_UI_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 整页渲染当前词卡（page_router base render 入口；空序列/
 *  空词库自显空态页）。释义多页时渲染当前页。刷新档入口自动判。 */
void word_card_ui_render(void);

/**
 * @brief 释义多页词内翻页（上/下短按先走此口，词内翻完再翻词）。
 * @param dir +1 下一页 / -1 上一页。
 * @return true 已翻页（本次按键消费于此）；false 已在边缘（编排层
 *         接 study_mode_handle_action 翻词）。
 */
bool word_card_ui_mean_page_step(int dir);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_WORD_CARD_UI_H */
