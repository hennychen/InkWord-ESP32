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
 * @brief 墨封圆形印章（单帧 ~100ms，按键回调上下文）。
 *
 * 单帧直接画完整印章：双同心圆环（外 r=52 w=3 / 内 r=44 w=2）
 * + 断线纹理（压印质感）+ 印泥黑点 + 中心「熟」字。
 * 盖在词卡上，调用方 after_master + render_top 翻页覆盖。
 * 仅 master 置位方向调用——启封不播动画。
 *
 * 调用方应在印章后调 study_mode_after_master() + render_top()
 * 自动跳转下词。
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
