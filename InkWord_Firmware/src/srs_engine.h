/**
 * @file srs_engine.h
 * @brief SRS 间隔重复算法引擎 (Task F-15)
 *
 * 实现 SM-2 算法变体：根据回忆质量分更新 EaseFactor 与下次复习间隔。
 * 纯算法，无硬件/RTOS 依赖，便于单元测试。
 */
#ifndef INKWORD_SRS_ENGINE_H
#define INKWORD_SRS_ENGINE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 回忆质量分（SM-2 标准 0~5） */
typedef enum {
    SRS_QUALITY_BLACKOUT   = 0,  /**< 完全没印象 */
    SRS_QUALITY_WRONG      = 1,  /**< 错误，但看到答案觉得熟悉 */
    SRS_QUALITY_WRONG_EASY = 2,  /**< 错误，但答案很容易记住 */
    SRS_QUALITY_CORRECT_HD = 3,  /**< 正确，但花了很大力气 */
    SRS_QUALITY_CORRECT    = 4,  /**< 正确，有少量犹豫 */
    SRS_QUALITY_PERFECT    = 5,  /**< 完美回忆 */
} srs_quality_t;

/** 一个单词的 SRS 状态节点 */
typedef struct {
    float    ease_factor;   /**< 难度系数，初始 2.5，范围 [1.3, 2.8] */
    uint16_t repetition;    /**< 连续正确次数 */
    uint16_t interval_days; /**< 当前间隔（天） */
    int64_t  next_review;   /**< 下次复习的 Unix 时间戳（秒） */
    uint8_t  srs_level;     /**< SRS 等级 0~5，用于统计分布 */
} SrsNode;

/**
 * @brief 初始化一个新词的 SRS 节点。
 */
void srs_init_node(SrsNode *node, int64_t now_unix);

/**
 * @brief 根据质量分计算并更新节点状态。 (F-15)
 * @param quality 回忆质量分。
 * @param node    待更新的节点（in/out）。
 * @param now_unix 当前 Unix 时间戳（秒）。
 * @return 更新后的间隔天数。
 */
uint16_t srs_calculate_next_review(srs_quality_t quality, SrsNode *node, int64_t now_unix);

/**
 * @brief 该节点当前是否已到期需复习。
 */
bool srs_is_due(const SrsNode *node, int64_t now_unix);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_SRS_ENGINE_H */
