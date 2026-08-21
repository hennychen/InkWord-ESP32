/**
 * @file learning_state.h
 * @brief 本地学习状态层 (P1 错词本 + 收藏；P2 加上报事件队列)
 *
 * 每词 SM-2 状态 + 连错计数 + 收藏标志。词库扩容（2026-08-20）后
 * 持久化改为 sparse 格式（LR02）：NVS blob 只存非默认态词
 * （学过/连错>0/已收藏），配 nvs 24KB 分区约束；词库规模变化仍
 * 整体作废（保守策略）。保存时机：评分/收藏仅置脏标记，主循环
 * learning_state_maybe_save() 静默 5s 后落盘（按键路径零 NVS 阻塞，
 * 掉电窗口 ≤5s，与事件队列不持久化策略一致）。
 *
 * 连错维护规则与后端 SrsService.ApplyReview 严格同步：
 *   quality < 3 连错递增（>0 即入错词本），quality >= 3 清零移出。
 *
 * 上报队列（P2）：评分/收藏动作同时入内存环形队列，由 main.cpp
 * background_task 联网时逐条 flush 到 sync_client（词须有 cloudId，
 * 即后端 /admin/words/export 导出的 Guid；本地导入词自动跳过）。
 * 队列不持久化：重启丢失未上报事件，但 NVS 已存最终态，云端仅
 * 少中间事件（连错/收藏为幂等 set，最终一致）。
 */
#ifndef INKWORD_LEARNING_STATE_H
#define INKWORD_LEARNING_STATE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LEARNING_STATE_MAX  (4000) /**< 状态容量上限 = 词池上限 MAX_WORDS；
                                         状态数组 PSRAM 分配（4000×~24B≈96KB），
                                         词库扩容落地见 PRD_V2.1 7.2 */

/** 上报队列事件（peek 输出用） */
typedef struct {
    int  word_idx;              /**< 词库索引 */
    int  quality;               /**< 0~5；<0 = 收藏事件 */
    bool collected;             /**< 仅收藏事件有效（幂等 set） */
} lr_event_t;

/**
 * @brief 初始化：锁定跟踪词数并从 NVS 恢复状态。
 *        词库词条数变化时旧状态整体作废（保守策略）。
 * @param word_count 词库实际词条数（超出 LEARNING_STATE_MAX 截断）。
 */
void learning_state_init(int word_count);

/**
 * @brief 应用一次 SM-2 评分（0~5）：更新该词 SRS 节点与连错计数。
 *        quality<3 连错+1，>=3 清零；置脏标记由主循环延迟落盘。
 */
void learning_state_apply_quality(int word_idx, int quality);

/**
 * @brief 收藏/取消收藏，返回切换后的状态；置脏标记延迟落盘。
 */
bool learning_state_toggle_collect(int word_idx);

/**
 * @brief 当前词是否已收藏。
 */
bool learning_state_is_collected(int word_idx);

/**
 * @brief 错词数量（consecutive_wrong > 0 的词条数）。
 */
int learning_state_wrong_count(void);

/**
 * @brief 错词视图取词：第 pos 个错词的词库索引（按索引序），越界返回 -1。
 */
int learning_state_wrong_at(int pos);

/**
 * @brief 全量写入 NVS（sparse 格式；评分/收藏自动置脏，一般无需外部调）。
 */
void learning_state_save(void);

/**
 * @brief 主循环周期调用：脏标记静默 LR_SAVE_DELAY 秒后自动落盘
 *        （评分/收藏仅置脏不落盘，按键路径零 NVS 写阻塞；
 *        连续操作刷新静默计时，学习完一轮后一次性写入）。
 */
void learning_state_maybe_save(void);

/* ---- 上报事件队列（P2 云端闭环，main.cpp background_task 消费） ---- */

/**
 * @brief 队列内待上报事件数（评分/收藏动作自动入队，满 24 覆盖最旧）。
 */
int learning_state_event_count(void);

/**
 * @brief 看队列第 i 条（0 = 最旧，i < event_count）。
 * @return false 索引越界。
 */
bool learning_state_event_peek(int i, lr_event_t *out);

/**
 * @brief 丢弃最旧 n 条（上报成功/确认无上报价值后调用）。
 */
void learning_state_event_drop(int n);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_LEARNING_STATE_H */
