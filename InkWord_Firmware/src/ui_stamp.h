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
 * @brief 墨封旋转盖章动画（同步阻塞 ~1.3s，按键回调上下文）。
 *
 * 4 帧节拍式：①小方印胚（16px 实心）→ ②菱形旋转 45°（36px 空心）→
 * ③大方形旋转回 0°（56px 空心）→ ④终印落地（80px 实心「熟」）→
 * 清白交调用方 render_top。方/菱交替 = 视觉旋转，尺寸递增 = 从小
 * 到大盖章。仅 master 置位方向调用——启封不播动画（正负反馈不对称）。
 *
 * 动画期间按键由 button_handler 队列缓冲，动画后按新状态正常处理。
 * 调用方应在动画后调 study_mode_after_master() + render_top() 自动
 * 跳转下词（避免动画后白屏）。
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
