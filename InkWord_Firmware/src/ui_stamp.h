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
 * @brief 墨封落印动画（同步阻塞 0.4~1.7s，按键回调上下文）。
 *
 * 三幕节拍：①反白方印「熟」盖正文区中央偏右 + 重震 + 音效「咚」
 * ②（仅快屏）印面微收正 ③收印清白（由调用方 render_top 重绘词卡
 * 带角标终结）。幕数按面板局刷速度运行期降级（快屏 3 幕 ~1.7s /
 * 标准局刷 2 幕 / 慢局刷与三色屏 1 幕）。
 *
 * 动画期间按键由 button_handler 队列缓冲，动画后按新状态正常处理
 * （quiz 反馈帧同策略，不做吞除）。仅 master 置位方向调用——
 * 启封不播动画（正负反馈不对称：奖励隆重、恢复轻简）。
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
