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
extern void test_word_parser_ignores_protocol_v2_fields(void);

/* quiz_session 出题核心（v1.2 T2.1，2026-08-24；v1.5 T5.1 P2 用例）：
 * 同 runner 挂载 */
extern void test_quiz_start_bounds(void);
extern void test_quiz_sampling_no_repeat(void);
extern void test_quiz_answer_mapping(void);
extern void test_quiz_all_same_text_fallback(void);
extern void test_quiz_rng_reproducible(void);
extern void test_quiz_type_rotation(void);
extern void test_quiz_same_prefix_distractors(void);
extern void test_quiz_true_false(void);

/* card_layout 版式分派（v1.4 T4.3）：同 runner 挂载 */
extern void test_card_layout_known_types(void);
extern void test_card_layout_fallbacks(void);

/* 显示链路几何真值表（T2.1 epd_geom 纯函数层）：同 runner 挂载 */
extern void test_epd_geom_transpose_four_dirs(void);
extern void test_epd_geom_transpose_roundtrip(void);
extern void test_epd_geom_rect_four_dirs(void);
extern void test_epd_geom_rect_clamp(void);
extern void test_epd_geom_palette_degrade(void);

/* 黄金帧 diff 灵敏度（T2.2 selftest_diff 纯函数层） ：同 runner 挂载 */
extern void test_selftest_diff_identical(void);
extern void test_selftest_diff_single_byte(void);
extern void test_selftest_diff_multi_first_offset(void);
extern void test_selftest_diff_null_off_safe(void);
/* T3.2 mask 纯函数：区域差异不计数/非区域计灵敏/置白/越界钳位 */
extern void test_selftest_mask_region_diff_ignored(void);
extern void test_selftest_mask_outside_diff_kept(void);
extern void test_selftest_mask_whitens_region(void);
extern void test_selftest_mask_rect_clamped(void);
/* LAN 直传协议 v2 帧分类/头解析（T2.3 lan_proto 纯函数层） */
extern void test_lan_classify_color_panel_paths(void);
extern void test_lan_classify_bw_panel_degrades(void);
extern void test_lan_v2_header_valid_bw_and_color(void);
extern void test_lan_v2_header_rejections(void);
/* 布局档位分派（P2c layout_profile 纯函数层，epd_gfx 桩驱动） */
extern void test_layout_dispatch_boundaries(void);
extern void test_layout_rotation_invariant(void);
extern void test_layout_narrow_tiny(void);
extern void test_layout_form_axis(void);
extern void test_layout_cached_pointer_stable(void);
extern void test_layout_profile_sanity_all_kinds(void);

/* poem 默写数据通路（v1.4 T4.4）：同 runner 挂载 */
extern void test_word_parser_poem_dictation_fields(void);

/* SD 卡组子集字库级联（v1.4 T4.5）：同 runner 挂载 */
extern void test_cjk_font_sd_load_and_lookup(void);
extern void test_cjk_font_sd_rejects_bad_bin(void);
extern void test_cjk_font_sd_missing_file_semantics(void);
extern void test_cjk_font_sd_legacy_3levels_compat(void);
extern void test_cjk_font_sd_swift_artifact(void);

/* catalog_index 教材目录索引（2026-08-28 教材目录浏览设计）：同 runner 挂载
 * （词池由 catalog_build_from 自构，不依赖 word_parser 运行期） */
extern void test_catalog_unit_name_of(void);
extern void test_catalog_grade_buckets_and_order(void);
extern void test_catalog_unit_order_numeric(void);
extern void test_catalog_unit_topic_and_starter_order(void);
extern void test_catalog_grade_order_expansion_and_primary(void);
extern void test_catalog_empty_and_all_blank_grade(void);
extern void test_catalog_overflow_merges_to_fallback(void);
extern void test_catalog_rebuild_idempotent(void);
extern void test_catalog_utf8_truncation_safe(void);

/* daily_plan 按组配额 + 考试倒计时（v1.5 T5.5）：同 runner 挂载
 * （nvs/时钟/组 id 桩与 mock 实现在 test_daily_plan.c） */
extern void test_dp_goal_default_when_no_key(void);
extern void test_dp_goal_deck_key_dispatch(void);
extern void test_dp_goal_invalid_value_falls_back(void);
extern void test_dp_set_goal_clamps_and_steps(void);
extern void test_dp_set_goal_writes_active_deck_key(void);
extern void test_dp_done_uses_deck_numerator(void);
extern void test_dp_done_mastered_exhausts_new(void);
extern void test_exam_unset_returns_zero(void);
extern void test_exam_set_and_countdown(void);
extern void test_exam_urgent_boundary(void);
extern void test_exam_expires_to_zero(void);
extern void test_exam_clear(void);
extern void test_exam_refused_when_clock_unsynced(void);
extern void test_exam_month_rollover_math(void);
extern void test_exam_leap_year_math(void);

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fsrs_replay_vectors);
    RUN_TEST(test_fsrs_anchors);
    RUN_TEST(test_parse_cloud_export_words_json);
    RUN_TEST(test_word_parser_load_mem);
    RUN_TEST(test_word_parser_ignores_protocol_v2_fields);
    RUN_TEST(test_quiz_start_bounds);
    RUN_TEST(test_quiz_sampling_no_repeat);
    RUN_TEST(test_quiz_answer_mapping);
    RUN_TEST(test_quiz_all_same_text_fallback);
    RUN_TEST(test_quiz_rng_reproducible);
    RUN_TEST(test_quiz_type_rotation);
    RUN_TEST(test_quiz_same_prefix_distractors);
    RUN_TEST(test_quiz_true_false);
    RUN_TEST(test_card_layout_known_types);
    RUN_TEST(test_card_layout_fallbacks);
    RUN_TEST(test_word_parser_poem_dictation_fields);
    RUN_TEST(test_cjk_font_sd_load_and_lookup);
    RUN_TEST(test_cjk_font_sd_rejects_bad_bin);
    RUN_TEST(test_cjk_font_sd_missing_file_semantics);
    RUN_TEST(test_cjk_font_sd_legacy_3levels_compat);
    RUN_TEST(test_cjk_font_sd_swift_artifact);
    RUN_TEST(test_dp_goal_default_when_no_key);
    RUN_TEST(test_dp_goal_deck_key_dispatch);
    RUN_TEST(test_dp_goal_invalid_value_falls_back);
    RUN_TEST(test_dp_set_goal_clamps_and_steps);
    RUN_TEST(test_dp_set_goal_writes_active_deck_key);
    RUN_TEST(test_dp_done_uses_deck_numerator);
    RUN_TEST(test_dp_done_mastered_exhausts_new);
    RUN_TEST(test_exam_unset_returns_zero);
    RUN_TEST(test_exam_set_and_countdown);
    RUN_TEST(test_exam_urgent_boundary);
    /* T2.1：显示链路几何真值表（epd_geom 纯函数层） */
    RUN_TEST(test_epd_geom_transpose_four_dirs);
    RUN_TEST(test_epd_geom_transpose_roundtrip);
    RUN_TEST(test_epd_geom_rect_four_dirs);
    RUN_TEST(test_epd_geom_rect_clamp);
    RUN_TEST(test_epd_geom_palette_degrade);
    RUN_TEST(test_exam_expires_to_zero);
    RUN_TEST(test_exam_clear);
    RUN_TEST(test_exam_refused_when_clock_unsynced);
    RUN_TEST(test_exam_month_rollover_math);
    RUN_TEST(test_exam_leap_year_math);
    RUN_TEST(test_catalog_unit_name_of);
    RUN_TEST(test_catalog_grade_buckets_and_order);
    RUN_TEST(test_catalog_unit_order_numeric);
    RUN_TEST(test_catalog_unit_topic_and_starter_order);
    RUN_TEST(test_catalog_grade_order_expansion_and_primary);
    RUN_TEST(test_catalog_empty_and_all_blank_grade);
    RUN_TEST(test_catalog_overflow_merges_to_fallback);
    RUN_TEST(test_catalog_rebuild_idempotent);
    RUN_TEST(test_catalog_utf8_truncation_safe);
    RUN_TEST(test_selftest_diff_identical);
    RUN_TEST(test_selftest_diff_single_byte);
    RUN_TEST(test_selftest_diff_multi_first_offset);
    RUN_TEST(test_selftest_diff_null_off_safe);
    RUN_TEST(test_selftest_mask_region_diff_ignored);
    RUN_TEST(test_selftest_mask_outside_diff_kept);
    RUN_TEST(test_selftest_mask_whitens_region);
    RUN_TEST(test_selftest_mask_rect_clamped);
    RUN_TEST(test_lan_classify_color_panel_paths);
    RUN_TEST(test_lan_classify_bw_panel_degrades);
    RUN_TEST(test_lan_v2_header_valid_bw_and_color);
    RUN_TEST(test_lan_v2_header_rejections);
    RUN_TEST(test_layout_dispatch_boundaries);
    RUN_TEST(test_layout_rotation_invariant);
    RUN_TEST(test_layout_narrow_tiny);
    RUN_TEST(test_layout_form_axis);
    RUN_TEST(test_layout_cached_pointer_stable);
    RUN_TEST(test_layout_profile_sanity_all_kinds);
    return UNITY_END();
}
