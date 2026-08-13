/**
 * @file srs_engine.c
 * @brief SRS 间隔重复算法实现 (Task F-15) —— SM-2 算法变体
 *
 * SM-2 核心：
 *   EF' = EF + (0.1 - (5 - q)*(0.08 + (5 - q)*0.02))，下限 1.3
 *   若 q < 3（答错）：repetition 归零，interval = 1
 *   若 q >= 3（答对）：
 *       repetition == 0 -> interval = 1
 *       repetition == 1 -> interval = 6
 *       repetition >= 2 -> interval = round(interval * EF)
 *
 * 连续正确复习，间隔序列约 1 -> 6 -> 15 -> 37 ... 天，
 * EF 随答题质量在 [1.3, 2.8] 浮动。验收口径见 test_srs_engine。
 */
#include "srs_engine.h"
#include <math.h>

#define SECONDS_PER_DAY  (24LL * 3600LL)
#define MIN_EF           (1.3f)
#define MAX_EF           (2.8f)
#define INIT_EF          (2.5f)

static float clamp_ef(float ef)
{
    if (ef < MIN_EF) return MIN_EF;
    if (ef > MAX_EF) return MAX_EF;
    return ef;
}

void srs_init_node(SrsNode *node, int64_t now_unix)
{
    if (!node) return;
    node->ease_factor   = INIT_EF;
    node->repetition    = 0;
    node->interval_days = 0;
    node->next_review   = now_unix;   /* 新词立即可学 */
    node->srs_level     = 0;
}

uint16_t srs_calculate_next_review(srs_quality_t quality, SrsNode *node, int64_t now_unix)
{
    if (!node) return 0;
    int q = (int)quality;

    /* 1. 更新 EaseFactor（与答对答错无关，都参与） */
    float delta = 0.1f - (5 - q) * (0.08f + (5 - q) * 0.02f);
    node->ease_factor = clamp_ef(node->ease_factor + delta);

    if (q < 3) {
        /* 答错：重置进度，明天再见 */
        node->repetition    = 0;
        node->interval_days = 1;
        node->srs_level     = 0;
    } else {
        /* 答对：递进间隔 */
        if (node->repetition == 0) {
            node->interval_days = 1;
        } else if (node->repetition == 1) {
            node->interval_days = 6;
        } else {
            node->interval_days = (uint16_t)llroundf((float)node->interval_days * node->ease_factor);
            if (node->interval_days < 1) node->interval_days = 1;
        }
        node->repetition++;
        node->srs_level = (node->repetition > 5) ? 5 : (uint8_t)node->repetition;
    }

    /* 计算下次复习时间戳 */
    node->next_review = now_unix + (int64_t)node->interval_days * SECONDS_PER_DAY;
    return node->interval_days;
}

bool srs_is_due(const SrsNode *node, int64_t now_unix)
{
    if (!node) return false;
    return now_unix >= node->next_review;
}
