/**
 * @file srs_engine.h
 * @brief SRS 间隔重复算法引擎 (Task F-15) —— FSRS-4.5（M4 路径 A，2026-08-22）
 *
 * 由 SM-2 重写为 FSRS-4.5（open-spaced-repetition 17 参数默认权重）。
 * 与后端 InkWord.Services/FsrsService.cs 严格同源镜像：quality 0~5 映射
 * rating 0-1→Again 2→Hard 3-4→Good 5→Easy；双端共用
 * tools/fsrs_test_vectors.csv 对拍（容差：S/D 相对 1e-5（float 存储
 * 舍入预算），间隔整数精确相等）。
 *
 * 纯算法，无硬件/RTOS 依赖，便于单元测试。
 * 调用方（learning_state/main）接口签名零改动；存储格式变更见
 * learning_state.c LR03。
 */
#ifndef INKWORD_SRS_ENGINE_H
#define INKWORD_SRS_ENGINE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 回忆质量分（SM-2 标准 0~5，FSRS 内部映射四档 rating） */
typedef enum {
    SRS_QUALITY_BLACKOUT   = 0,  /**< 完全没印象 → Again */
    SRS_QUALITY_WRONG      = 1,  /**< 错误，但看到答案觉得熟悉 → Again */
    SRS_QUALITY_WRONG_EASY = 2,  /**< 错误，但答案很容易记住 → Hard */
    SRS_QUALITY_CORRECT_HD = 3,  /**< 正确，但花了很大力气 → Good */
    SRS_QUALITY_CORRECT    = 4,  /**< 正确，有少量犹豫 → Good */
    SRS_QUALITY_PERFECT    = 5,  /**< 完美回忆 → Easy */
} srs_quality_t;

/** 一个单词的 FSRS 状态节点（约 25B，与旧 SM-2 节点同量级） */
typedef struct {
    float    stability;     /**< 记忆稳定性 S（天）；0 = 未学（惰性初始化） */
    float    difficulty;    /**< 难度 D ∈ [1,10] */
    int64_t  last_review;   /**< 上次复习 Unix 时间戳（秒，相对钟域由调用方定义） */
    int64_t  next_review;   /**< 下次复习 Unix 时间戳（秒） */
    uint8_t  srs_level;     /**< SRS 等级 0~5，用于统计分布（规则与后端同步） */
} SrsNode;

/**
 * @brief 初始化一个新词的 SRS 节点（未学态：S=0，首评走初始化分支）。
 */
void srs_init_node(SrsNode *node, int64_t now_unix);

/**
 * @brief 按质量分推进 FSRS 状态（S/D 更新 + 间隔排期）。 (F-15/M4)
 * @param quality 回忆质量分。
 * @param node    待更新的节点（in/out；未学态自动初始化 S0/D0）。
 * @param now_unix 当前 Unix 时间戳（秒）。
 * @return 更新后的间隔天数（1~65535）。
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
