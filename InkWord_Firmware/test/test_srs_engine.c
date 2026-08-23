/**
 * @file test_srs_engine.c
 * @brief srs_engine FSRS-4.5 对拍测试（M4 路径 A，2026-08-22）
 *
 * 重放 tools/gen_fsrs_vectors.py 生成的 12 组复习序列（与后端
 * FsrsServiceTests 读同一 CSV 源），断言 S/D/interval/next_review
 * 与参考实现一致：S/D 相对容差 1e-5（SrsNode float 存储舍入预算，
 * 见 srs_engine.c 精度策略注释），间隔与到期时间戳精确相等。
 *
 * 运行：pio test -e native-test（host 编译，无硬件依赖）
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unity.h>
#include "srs_engine.h"
#include "fsrs_vectors.h"

/* 相对容差断言（失败消息带 case/step 定位） */
static void assert_rel(double expected, double actual, const fsrs_vec_t *v)
{
    double tol = fabs(expected) * 1e-5 + 1e-6;
    if (fabs(actual - expected) > tol) {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s[%d] S/D: exp=%.6f act=%.6f",
                 v->case_name, v->step, expected, actual);
        TEST_FAIL_MESSAGE(msg);
    }
}

void test_fsrs_replay_vectors(void)
{
    int i = 0, cases_checked = 0;
    while (i < FSRS_VEC_COUNT) {
        /* 同名连续段为一个 case，逐段重放（last_review 手动推进） */
        int j = i;
        while (j < FSRS_VEC_COUNT &&
               strcmp(kFsrsVectors[j].case_name, kFsrsVectors[i].case_name) == 0)
            j++;

        SrsNode node;
        int64_t last_review = 0;
        srs_init_node(&node, 0);

        for (int k = i; k < j; k++) {
            const fsrs_vec_t *v = &kFsrsVectors[k];
            int64_t now = last_review + (int64_t)v->elapsed_days * 86400LL;
            uint16_t iv = srs_calculate_next_review(
                (srs_quality_t)v->quality, &node, now);

            /* 固件 uint16_t 返回预算：期望值同步 clamp 后再对拍
             * （easy 连评 6 次后 S 远超人类寿命，仅截断值差异） */
            int exp_iv = v->interval > 65535 ? 65535 : v->interval;

            char label[96];
            snprintf(label, sizeof(label), "%s[%d] interval",
                     v->case_name, v->step);
            TEST_ASSERT_EQUAL_INT_MESSAGE(exp_iv, (int)iv, label);

            assert_rel(v->s, (double)node.stability, v);
            assert_rel(v->d, (double)node.difficulty, v);

            snprintf(label, sizeof(label), "%s[%d] next_review",
                     v->case_name, v->step);
            TEST_ASSERT_EQUAL_INT64_MESSAGE(
                now + (int64_t)exp_iv * 86400LL, node.next_review, label);

            last_review = now;
        }
        cases_checked++;
        i = j;
    }
    /* 向量完整性：12 组全跑（防止向量文件损坏静默缩水） */
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(12, cases_checked,
                                             "vector cases missing");
}

void test_fsrs_anchors(void)
{
    /* 首评 Good：S0=W[2]=3.7145，D0=clamp(5.1618-e^1.795)→1，
     * interval=round(S)=4（目标留存 0.90 不变量） */
    SrsNode node;
    srs_init_node(&node, 1000);
    uint16_t iv = srs_calculate_next_review(SRS_QUALITY_CORRECT_HD, &node, 1000);
    TEST_ASSERT_EQUAL_FLOAT(3.7145f, node.stability);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, node.difficulty);
    TEST_ASSERT_EQUAL_UINT16(4, iv);
    TEST_ASSERT_EQUAL_INT64(1000 + 4LL * 86400, node.next_review);

    /* 等级规则与后端同步：q>=3 递增，q<3 归零 */
    TEST_ASSERT_EQUAL_UINT8(1, node.srs_level);
    srs_calculate_next_review(SRS_QUALITY_BLACKOUT, &node, 1000);
    TEST_ASSERT_EQUAL_UINT8(0, node.srs_level);

    /* 未初始化/新词立即可学（初始化时刻即到期） */
    SrsNode fresh;
    srs_init_node(&fresh, 500);
    TEST_ASSERT_TRUE(srs_is_due(&fresh, 500));
    TEST_ASSERT_FALSE(srs_is_due(&fresh, 499));
}

/* word_parser 云导出回放（2026-08-23）：WORDS_JSON 环境变量喂真实导出，
 * 缺省 IGNORE。native 平台单 program 单 main，统一挂在本 runner。 */
extern void test_parse_cloud_export_words_json(void);
extern void test_word_parser_load_mem(void);

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fsrs_replay_vectors);
    RUN_TEST(test_fsrs_anchors);
    RUN_TEST(test_parse_cloud_export_words_json);
    RUN_TEST(test_word_parser_load_mem);
    return UNITY_END();
}
