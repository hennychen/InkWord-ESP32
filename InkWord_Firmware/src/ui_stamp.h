/**
 * @file ui_stamp.h
 * @brief 墨封落印动画 + 词卡「熟」角标（2026-09-04 墨封功能）
 *
 * 文案体系「墨封/启封/墨封录」：印章认证意象的 UI 侧呈现，
 * 技术符号 mastered（learning_state）。
 */
#ifndef INKWORD_UI_STAMP_H
#define INKWORD_UI_STAMP_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 墨封圆形盖章动画（同步阻塞 ~450ms，按键回调上下文）。
 *
 * 3 帧节拍式：①小圆点（12px）→ ②中圆（36px）→ ③大圆印「熟」（80px）。
 * 尺寸递增 = 从小到大盖章。末帧保留圆印，调用方 render_top 渲染新词
 * 直接覆盖（无白屏过渡）。仅 master 置位方向调用——启封不播动画。
 *
 * 动画期间按键由 button_handler 队列缓冲，动画后按新状态正常处理。
 * 调用方应在动画后调 study_mode_after_master() + render_top() 自动
 * 跳转下词。
 */
void ui_stamp_play(void);

/**
 * @brief 词卡墨封角标：20px 空心方印 + 16px「熟」居中（印章边框意象）。
 * @param right_x 角标右缘 x（右对齐锚点，与收藏 * 拼排时传其左侧）。
 * @param y       16px 行顶左基准（框上提 2px 垂直居中，音标行同级）。
 */
void ui_draw_seal_mark(int right_x, int y);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_UI_STAMP_H */
