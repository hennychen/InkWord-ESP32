/**
 * @file srs_engine.c
 * @brief SRS 间隔重复算法实现 —— FSRS-4.5（M4 路径 A，2026-08-22）
 *
 * 公式与后端 FsrsService.cs / tools/gen_fsrs_vectors.py 严格同源
 * （open-spaced-repetition FSRS-4.5 默认 17 参数权重）：
 *   S0(r) = W[r-1]；D0(r) = clamp(W4 - e^(W5*(r-1)), 1, 10)
 *   R(t,S) = (1 + FACTOR*t/S)^DECAY，DECAY=-0.5，FACTOR=19/81
 *   遗忘(r=1)：S' = W11*D^(-W12)*((S+1)^W13 - 1)*e^(W14*(1-R))
 *   回忆(r≥2)：S' = S*(1 + e^W8*(11-D)*S^(-W9)*(e^(W10*(1-R))-1)*HP*EB)
 *     HP = W15 (r=Hard)，EB = W16 (r=Easy)
 *   D' = clamp(D - W6*(r-3), 1, 10)
 *   interval = max(1, round(S*(R*^(1/DECAY)-1)/FACTOR))，R*=0.90 时 = round(S)
 *
 * 精度策略：中间量 double（S3 无 double FPU，但评分是低频人机交互操作，
 * 每次 ~20 次超越函数运算性能无感），节点存储 float（PSRAM 状态数组与
 * NVS blob 尺寸预算）。对拍容差 1e-5 相对（float 舍入累积预算）。
 *
 * srs_level 规则与后端 FsrsService.ApplyReview 同步：
 * q>=3 → min(5, level+1)，否则 0。
 */
#include "srs_engine.h"
#include <math.h>

#define SECONDS_PER_DAY  (24LL * 3600LL)
#define INTERVAL_MAX     (65535)   /* uint16_t 返回值保护（~179 年，实际远不可达 */

/* FSRS-4.5 默认权重（open-spaced-repetition 官方值，勿随意改动——双端对拍基准） */
static const double W[17] = {
    0.4872, 1.4003, 3.7145, 13.8206, 5.1618, 1.2298, 0.8975, 0.031,
    1.6474, 0.1367, 1.0461, 2.1072, 0.0793, 0.3246, 1.587, 0.2272, 2.8755,
};

#define FSRS_DECAY   (-0.5)
#define FSRS_FACTOR  (19.0 / 81.0)
#define FSRS_R_STAR  (0.90)   /* 目标留存率：0.90 下间隔恰等于 S */

static double clampd(double v, double lo, double hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/** quality 0~5 → rating 1=Again 2=Hard 3=Good 4=Easy（后端 MapRating 同映射） */
static int map_rating(int quality)
{
    if (quality <= 1) return 1;
    if (quality == 2) return 2;
    if (quality >= 5) return 4;
    return 3;
}

/** 可提取性 R(t,S) */
static double retrievability(double s, double elapsed_days)
{
    return pow(1.0 + FSRS_FACTOR * elapsed_days / s, FSRS_DECAY);
}

/** 目标留存率下的复习间隔（天） */
static int next_interval(double s)
{
    double iv = s * (pow(FSRS_R_STAR, 1.0 / FSRS_DECAY) - 1.0) / FSRS_FACTOR;
    long n = lround(iv);
    if (n < 1) n = 1;
    if (n > INTERVAL_MAX) n = INTERVAL_MAX;
    return (int)n;
}

void srs_init_node(SrsNode *node, int64_t now_unix)
{
    if (!node) return;
    node->stability   = 0.0f;   /* 未学：首评走 S0/D0 初始化分支 */
    node->difficulty  = 0.0f;
    node->last_review = now_unix;
    node->next_review = now_unix;   /* 新词立即可学 */
    node->srs_level   = 0;
}

uint16_t srs_calculate_next_review(srs_quality_t quality, SrsNode *node, int64_t now_unix)
{
    if (!node) return 0;
    int r = map_rating((int)quality);

    double s = (double)node->stability;
    double d = (double)node->difficulty;

    if (s <= 0.0) {
        /* 首评：S0/D0 初始化 */
        s = W[r - 1];
        d = clampd(W[4] - exp(W[5] * (r - 1)), 1.0, 10.0);
    } else {
        double elapsed = (double)(now_unix - node->last_review) / (double)SECONDS_PER_DAY;
        if (elapsed < 0.0) elapsed = 0.0;
        double R = clampd(retrievability(s, elapsed), 0.005, 0.999);

        if (r == 1) {
            /* 遗忘：S_f = W11*D^(-W12)*((S+1)^W13 - 1)*e^(W14*(1-R)) */
            s = W[11] * pow(d, -W[12])
                * (pow(s + 1.0, W[13]) - 1.0)
                * exp(W[14] * (1.0 - R));
        } else {
            /* 回忆：HP/EB 修正 */
            double hp = (r == 2) ? W[15] : 1.0;
            double eb = (r == 4) ? W[16] : 1.0;
            s *= 1.0 + exp(W[8]) * (11.0 - d) * pow(s, -W[9])
                 * (exp(W[10] * (1.0 - R)) - 1.0) * hp * eb;
        }

        if (s < 0.01) s = 0.01;
        d = clampd(d - W[6] * (r - 3), 1.0, 10.0);
    }

    int interval = next_interval(s);

    node->stability  = (float)s;
    node->difficulty = (float)d;
    node->last_review = now_unix;
    node->next_review  = now_unix + (int64_t)interval * SECONDS_PER_DAY;

    /* 等级规则与后端 FsrsService.ApplyReview 同步 */
    if ((int)quality >= 3)
        node->srs_level = (node->srs_level < 5) ? (uint8_t)(node->srs_level + 1) : 5;
    else
        node->srs_level = 0;

    return (uint16_t)interval;
}

bool srs_is_due(const SrsNode *node, int64_t now_unix)
{
    if (!node) return false;
    return now_unix >= node->next_review;
}
