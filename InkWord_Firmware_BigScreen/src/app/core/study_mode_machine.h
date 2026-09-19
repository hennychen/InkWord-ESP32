/**
 * @file study_mode_machine.h
 * @brief 学习模式状态机 —— 大屏核心闭环精简版（小屏 Task F-16 同名 API 子集）
 *
 * 模式：闪卡(FLASH) / 复习(REVIEW) / 错词本(WRONGBOOK) / 收藏浏览
 * (COLLECTION) / 墨封录(MASTERED)。语义动作由导航键映射（上下=翻词、
 * SET=揭晓、RST=回首；左/右自评由编排层接 learning_state）。
 *
 * 精简范围（迁移 Phase A+B 裁定，2026-11）：
 *   - 保留：FLASH/REVIEW 序列逻辑与 WRONGBOOK/COLLECTION/MASTERED
 *     临时视图（learning_state 过滤视图全量复用）、遮蔽/揭晓、
 *     last_mode NVS 恢复、after_* 收缩钳位家族、seek。
 *   - 枚举值与小屏一一对应（0~10 契约稳定，MODE_COUNT 终值同）；
 *     DICTATION/READER 无音频/阅读器支撑：不进切换循环、不接受
 *     NVS 恢复与 study_mode_set（后续阶段接线时放开）。
 *   - CHAT/QUIZ/BROWSE/VOICE/PRON 未迁移：enter/exit 与跟读编排
 *     接口整体省略（无调用方；后续阶段按需补回同名 API）。
 *   - 临时视图纪律同小屏：不入循环/不 NVS 恢复/不走 apply_mode。
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
    MODE_DICTATION,      /**< 听写：听音拼写（大屏暂不可用，值位保留） */
    MODE_REVIEW,         /**< 复习：SRS 到期词 */
    MODE_READER,         /**< 阅读：整本书分页阅读（大屏暂不可用，值位保留） */
    MODE_WRONGBOOK,      /**< 错词本：连错词专项（RST 长按进出，不入切换循环） */
    MODE_COLLECTION,     /**< 收藏浏览：收藏词临时视图（枚举值固定 5） */
    MODE_CHAT,           /**< AI 对话（未迁移，值位保留） */
    MODE_QUIZ,           /**< 快速测验（未迁移，值位保留） */
    MODE_BROWSE,         /**< 教材目录浏览（未迁移，枚举值固定 8） */
    MODE_VOICE,          /**< 语音查词（未迁移，枚举值固定 9） */
    MODE_MASTERED,       /**< 墨封录：已墨封词临时视图（枚举值固定 10） */
    MODE_COUNT
} study_mode_t;

/** 初始化状态机：NVS 恢复上次模式（仅 FLASH/REVIEW 白名单），默认闪卡。 */
void study_mode_init(void);

/** 获取当前模式。 */
study_mode_t study_mode_current(void);

/** 循环切换到下一个模式（FLASH <-> REVIEW；返回切换后的模式）。 */
study_mode_t study_mode_switch_next(void);

/**
 * 直接设置模式（游标归零/遮蔽复位/持久化，与循环切换副作用一致；
 * 临时视图与暂不可用模式（DICTATION/READER）拒绝）。
 */
void study_mode_set(study_mode_t mode);

/** 获取当前模式的显示名称（UI 状态栏用）。 */
const char *study_mode_name(study_mode_t mode);

/**
 * 处理"上一条/下一条/确认"语义动作（按键事件经编排层映射后调用）。
 * @param action 0=prev 1=next 2=confirm(遮蔽/揭晓) 3=speak（大屏桩恒跳过）
 */
void study_mode_handle_action(int action);

/** 释义当前是否显示（默认 true；SET 翻义切换，翻词/切模式回显示）。 */
bool study_mode_is_revealed(void);

/** 光标重置到当前模式第一条并重绘（RST 短按）。 */
void study_mode_reset_cursor(void);

/* ---- 错词本临时视图（RST 长按进出；不持久化 last_mode） ---- */
bool study_mode_enter_wrongbook(void);
void study_mode_exit_wrongbook(void);

/* ---- 收藏浏览临时视图（收藏计数非零进；错词本同构） ---- */
bool study_mode_enter_collection(void);
void study_mode_exit_collection(void);

/* ---- 墨封录临时视图（收藏浏览同构镜像） ---- */
bool study_mode_enter_mastered(void);
void study_mode_exit_mastered(void);

/**
 * 词库索引定位：切 FLASH 模式 + 游标=index（钳位）+ 渲染。
 * 空词库无操作；目标词已墨封时先启封（跳转即启封语义同小屏）。
 */
void study_mode_seek(int word_index);

/**
 * 取消收藏（SET 长按 toggle）之后的序列收缩钳位：后词前移、游标钳
 * n-1；序列清空自动退回闪卡。
 * @return true 表示游标/模式变化，需重绘当前页。
 */
bool study_mode_after_uncollect(void);

/**
 * 评分应用后的错词本序列维护（quality>=3 的词移出错词序列，后词
 * 前移、游标回绕 0；序列清空自动退回闪卡）。
 * @return true 表示游标/模式变化，需重绘当前页。
 */
bool study_mode_after_quality(int quality);

/**
 * 墨封/启封后的序列收缩钳位（toggle_master 之后调用）：当前词移出
 * 所在序列，后词前移、游标钳 n-1；错词本/墨封录清空自动退回闪卡。
 * @return true 表示游标/模式变化，需重绘当前页。
 */
bool study_mode_after_master(void);

/**
 * 复习模式自评后的到期序列收缩（任一 quality 出队：本会话已过，
 * 避免原地循环）；序列清空钳 0 由渲染层显空态页。
 * @return true 表示序列变化，需重绘当前页。
 */
bool study_mode_after_due_review(void);

/** 当前显示词的词库索引（评分/收藏的目标词；过滤视图内为游标映射）。 */
int study_mode_current_word_index(void);

/** 当前模式序列位置（1 基显示用：pos+1/total）。 */
int study_mode_seq_pos(void);

/** 当前模式序列总长（错词本=错词数，闪卡/复习=对应过滤视图数）。 */
int study_mode_seq_total(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_STUDY_MODE_MACHINE_H */
