/**
 * @file study_mode_machine.h
 * @brief 学习模式状态机 (Task F-16；P1 增错词本)
 *
 * 模式：闪卡(FLASH) / 听写(DICTATION) / 复习(REVIEW) / 阅读(READER) /
 * 错词本(WRONGBOOK)。语义动作由五向导航键映射：上下=翻词/翻页，
 * 中=发音，SET=揭晓/确认，RST=回第一条；长按下=循环切换模式
 * （错词本为临时视图不入循环，由 RST 长按进出，见 V2.1 交互总表）。
 * 阅读模式（P3）下游标=页码，序列长度=总页数（reader_engine）；
 * 进入时自动恢复上次阅读页，左/右短按切字号（保持阅读位置）。
 * 序列抽象：默认全词库；错词本模式下序列换为 ConsecutiveWrong>0
 * 过滤视图（learning_state 提供），游标与取词均经 seq 接口。
 */
#ifndef INKWORD_STUDY_MODE_MACHINE_H
#define INKWORD_STUDY_MODE_MACHINE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MODE_FLASH = 0,      /**< 闪卡：看词猜义 */
    MODE_DICTATION,      /**< 听写：听音拼写 */
    MODE_REVIEW,         /**< 复习：SRS 到期词 */
    MODE_READER,         /**< 阅读：整本书分页阅读（P3；游标=页码，序列=页序列） */
    MODE_WRONGBOOK,      /**< 错词本：连错词专项（RST 长按进出，不入切换循环） */
    MODE_COUNT
} study_mode_t;

/**
 * @brief 初始化状态机，默认进入闪卡模式。
 */
void study_mode_init(void);

/**
 * @brief 获取当前模式。
 */
study_mode_t study_mode_current(void);

/**
 * @brief 循环切换到下一个模式。
 * @return 切换后的模式。
 */
study_mode_t study_mode_switch_next(void);

/**
 * @brief 直接设置模式。
 */
void study_mode_set(study_mode_t mode);

/**
 * @brief 获取当前模式的显示名称（用于 UI）。
 */
const char *study_mode_name(study_mode_t mode);

/**
 * @brief 在当前模式下处理“上一条/下一条/确认/发音”等语义动作。
 *        由按键事件经模式映射后调用。
 * @param action 0=prev 1=next 2=confirm(闪卡翻义/听写提交) 3=speak
 */
void study_mode_handle_action(int action);

/**
 * @brief 释义当前是否显示（默认 true；SET 翻义切换遮蔽/揭晓自测）。
 *        翻页/切模式后自动回到显示状态。
 */
bool study_mode_is_revealed(void);

/**
 * @brief 光标重置到当前模式第一条并重绘（RST 侧键）。
 */
void study_mode_reset_cursor(void);

/**
 * @brief 进入错词本（RST 长按；临时视图，不持久化 last_mode）。
 * @return true 成功；false 无错词（调用方提示边界反馈）。
 */
bool study_mode_enter_wrongbook(void);

/**
 * @brief 退出错词本回闪卡模式（错词本内 RST 长按）。
 */
void study_mode_exit_wrongbook(void);

/**
 * @brief 当前显示词的词库索引（评分/收藏的目标词）。
 *        错词本模式下为错词序列游标映射后的词库索引。
 */
int study_mode_current_word_index(void);

/**
 * @brief 当前模式序列位置（1 基显示用：pos+1/total）。
 */
int study_mode_seq_pos(void);

/**
 * @brief 当前模式序列总长（错词本=错词数，其余=词库总数）。
 */
int study_mode_seq_total(void);

/**
 * @brief 阅读模式字号切换（左/右短按）：切级 16/20/24px 循环，重建页表
 *        并按当前页首字符就近重定位游标后重绘。非阅读模式/无书时无操作。
 */
void study_mode_reader_font_step(int dir);

/**
 * @brief 评分应用后的错词本序列维护（learning_state_apply_quality 之后调用）。
 *        仅错词本模式有副作用：quality>=3 的词移出错词序列，后词前移、
 *        游标原位停留（越界回绕）；序列清空自动退回闪卡模式。
 * @return true 表示游标/模式变化，需重绘当前页。
 */
bool study_mode_after_quality(int quality);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_STUDY_MODE_MACHINE_H */
