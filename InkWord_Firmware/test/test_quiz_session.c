/**
 * @file test_quiz_session.c
 * @brief QUIZ 出题核心 native 测试（v1.2 T2.1，2026-08-24；
 *        v1.5 T5.1 P2 用例追加：题型轮换 / T3 降级 / 同前缀干扰）
 *
 * 覆盖（计划 T2.1 用例清单）：start 边界（pool<8 拒绝 / rounds 钳位 /
 * 空回调）、题干不放回抽样、选项索引互异 + 含正确项、选项文本两两
 * 去重、同义库兜底降级、作答评分映射与非法参数、RNG 全注入可复现。
 * T5.1：start_ex 题型奇偶轮换与 T3 逐词探测降级、同前缀阶段 A
 * 偏好命中与小组耗尽回退、slot 2 NULL 静默关闭。
 *
 * 运行：pio test -e native-test（native 平台单 program 单 main，
 * 用例统一挂 test_srs_engine.c 的 runner，本文件不建 main）。
 */
#include <stdio.h>
#include <string.h>
#include <unity.h>
#include "quiz_session.h"

/* ---- 注入：确定性 LCG（会话内一次播种，测试外重置） ---- */
static uint32_t s_seed = 1;
static uint32_t rnd_lcg(uint32_t bound)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return bound ? s_seed % bound : 0;
}

/* ---- 注入：文本回调（4 缓冲轮转，strcmp 双指针瞬时有效） ---- */
static char s_buf[4][32];
static int  s_buf_i = 0;
static const char *txt_seq(int idx, int slot)
{
    char *b = s_buf[s_buf_i = (s_buf_i + 1) & 3];
    snprintf(b, 32, "%s%d", slot == 0 ? "Q" : "A", idx);
    return b;
}

/* 全库同文本：文本去重无解，验证兜底放宽仍出齐索引互异选项 */
static const char *txt_same(int idx, int slot)
{
    (void)idx; (void)slot;
    return "same";
}

/* ---- v1.5 T5.1 注入：前缀词表 + T3 探测回调 ---- */

/* 前缀词表：词 0..9 前缀 "a" / 10..49 前缀 "b"（字面量稳定指针，
 * slot 2 单缓冲契约的最小反例——静态存储边不破） */
static const char *txt_pref(int idx, int slot)
{
    if (slot == 2) return idx < 10 ? "a" : "b";
    char *b = s_buf[s_buf_i = (s_buf_i + 1) & 3];
    snprintf(b, 32, "%s%d", slot == 0 ? "W" : "M", idx);
    return b;
}

/* 小组前缀：仅 0..2 为 "a"（同前缀候选 ≤2，阶段 A 必耗尽回退 B） */
static const char *txt_pref3(int idx, int slot)
{
    if (slot == 2) return idx < 3 ? "a" : "b";
    char *b = s_buf[s_buf_i = (s_buf_i + 1) & 3];
    snprintf(b, 32, "%s%d", slot == 0 ? "W" : "M", idx);
    return b;
}

/* slot 2 返回 NULL：同前缀偏好静默关闭（阶段 A 跳过不阻断） */
static const char *txt_prefnull(int idx, int slot)
{
    if (slot == 2) return NULL;
    char *b = s_buf[s_buf_i = (s_buf_i + 1) & 3];
    snprintf(b, 32, "%s%d", slot == 0 ? "W" : "M", idx);
    return b;
}

static int audio_all(int idx)  { (void)idx; return 1; }
static int audio_none(int idx) { (void)idx; return 0; }
static int audio_even_word(int idx) { return (idx % 2) == 0; }

void test_quiz_start_bounds(void)
{
    quiz_question_t q;

    /* 空回调拒绝 */
    TEST_ASSERT_EQUAL_INT(-1, quiz_session_start(100, 10, NULL, rnd_lcg));
    TEST_ASSERT_EQUAL_INT(-1, quiz_session_start(100, 10, txt_seq, NULL));
    /* 词池 < 8：干扰项来源不足拒绝 */
    TEST_ASSERT_EQUAL_INT(-1, quiz_session_start(7, 5, txt_seq, rnd_lcg));
    TEST_ASSERT_EQUAL_INT(-1, quiz_session_start(0, 5, txt_seq, rnd_lcg));
    /* rounds < 1 拒绝 */
    TEST_ASSERT_EQUAL_INT(-1, quiz_session_start(50, 0, txt_seq, rnd_lcg));
    /* 未开会话：at / answer 均无效 */
    TEST_ASSERT_FALSE(quiz_session_at(0, &q));
    TEST_ASSERT_EQUAL_INT(-1, quiz_session_answer(0, 0));

    /* rounds 钳位：超上限钳 32；超池钳到池大小 */
    TEST_ASSERT_EQUAL_INT(32, quiz_session_start(500, 100, txt_seq, rnd_lcg));
    TEST_ASSERT_EQUAL_INT(10, quiz_session_start(10, 40, txt_seq, rnd_lcg));
    TEST_ASSERT_EQUAL_INT(10, quiz_session_start(50, 10, txt_seq, rnd_lcg));
}

void test_quiz_sampling_no_repeat(void)
{
    s_seed = 42;
    TEST_ASSERT_EQUAL_INT(10, quiz_session_start(50, 10, txt_seq, rnd_lcg));

    quiz_question_t q;
    int stems[32];   /* 题干去重检查表（rounds 上限同核心静态题表） */
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_TRUE(quiz_session_at(i, &q));
        TEST_ASSERT_TRUE(q.word_idx >= 0 && q.word_idx < 50);
        TEST_ASSERT_TRUE(q.answer >= 0 && q.answer < QUIZ_OPTS);

        /* 选项索引互异、含正确项、answer 槽指向题干词 */
        bool has_stem = false;
        for (int k = 0; k < QUIZ_OPTS; k++) {
            TEST_ASSERT_TRUE(q.opt_word[k] >= 0 && q.opt_word[k] < 50);
            for (int j = k + 1; j < QUIZ_OPTS; j++)
                TEST_ASSERT_NOT_EQUAL_INT(q.opt_word[k], q.opt_word[j]);
            if (q.opt_word[k] == q.word_idx) has_stem = true;
        }
        TEST_ASSERT_TRUE(has_stem);
        TEST_ASSERT_EQUAL_INT(q.word_idx, q.opt_word[q.answer]);

        /* 选项文本两两不同（slot 1；拷贝后比较避免回调缓冲复用；
         * Unity 无 NOT_EQUAL_STRING 断言，strcmp 手写） */
        char texts[QUIZ_OPTS][32];
        for (int k = 0; k < QUIZ_OPTS; k++)
            snprintf(texts[k], 32, "%s", txt_seq(q.opt_word[k], 1));
        for (int k = 0; k < QUIZ_OPTS; k++)
            for (int j = k + 1; j < QUIZ_OPTS; j++)
                TEST_ASSERT_TRUE(strcmp(texts[k], texts[j]) != 0);

        /* 题干不放回抽样 */
        for (int j = 0; j < i; j++)
            TEST_ASSERT_NOT_EQUAL_INT(stems[j], q.word_idx);
        stems[i] = q.word_idx;
    }
    /* at 越界 */
    TEST_ASSERT_FALSE(quiz_session_at(10, &q));
    TEST_ASSERT_FALSE(quiz_session_at(-1, &q));
}

void test_quiz_answer_mapping(void)
{
    s_seed = 7;
    TEST_ASSERT_EQUAL_INT(10, quiz_session_start(50, 10, txt_seq, rnd_lcg));

    quiz_question_t q;
    TEST_ASSERT_TRUE(quiz_session_at(0, &q));
    int wrong = (q.answer + 1) % QUIZ_OPTS;

    /* 非法题号 / 槽位 */
    TEST_ASSERT_EQUAL_INT(-1, quiz_session_answer(-1, 0));
    TEST_ASSERT_EQUAL_INT(-1, quiz_session_answer(10, 0));
    TEST_ASSERT_EQUAL_INT(-1, quiz_session_answer(0, -1));
    TEST_ASSERT_EQUAL_INT(-1, quiz_session_answer(0, QUIZ_OPTS));

    /* 对 → 1；已答不可改 */
    TEST_ASSERT_EQUAL_INT(1, quiz_session_answer(0, q.answer));
    TEST_ASSERT_EQUAL_INT(-1, quiz_session_answer(0, wrong));

    /* 错 → 0 */
    TEST_ASSERT_TRUE(quiz_session_at(1, &q));
    TEST_ASSERT_EQUAL_INT(0, quiz_session_answer(1, (q.answer + 2) % QUIZ_OPTS));

    /* 跳过 = 不调 answer 直接换题：题 2 保持可答 */
    TEST_ASSERT_TRUE(quiz_session_at(2, &q));
    TEST_ASSERT_EQUAL_INT(1, quiz_session_answer(2, q.answer));
}

void test_quiz_all_same_text_fallback(void)
{
    s_seed = 99;
    /* 池 8 / 8 题：末题干扰项只能复用题干词（in_round 全排除），
     * 兜底路径必经；全库同文本同时压垮文本去重——两条放宽叠加 */
    TEST_ASSERT_EQUAL_INT(8, quiz_session_start(8, 8, txt_same, rnd_lcg));
    quiz_question_t q;
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_TRUE(quiz_session_at(i, &q));
        TEST_ASSERT_EQUAL_INT(q.word_idx, q.opt_word[q.answer]);
        for (int k = 0; k < QUIZ_OPTS; k++)
            for (int j = k + 1; j < QUIZ_OPTS; j++)
                TEST_ASSERT_NOT_EQUAL_INT(q.opt_word[k], q.opt_word[j]);
    }
}

void test_quiz_rng_reproducible(void)
{
    /* 同种子重放两次出题序列全等：RNG 全注入、核心无隐藏状态 */
    quiz_question_t a, b;
    s_seed = 1234;
    TEST_ASSERT_EQUAL_INT(10, quiz_session_start(100, 10, txt_seq, rnd_lcg));
    TEST_ASSERT_TRUE(quiz_session_at(0, &a));
    s_seed = 1234;
    TEST_ASSERT_EQUAL_INT(10, quiz_session_start(100, 10, txt_seq, rnd_lcg));
    TEST_ASSERT_TRUE(quiz_session_at(0, &b));
    TEST_ASSERT_EQUAL_INT(a.word_idx, b.word_idx);
    TEST_ASSERT_EQUAL_INT(a.answer, b.answer);
    for (int k = 0; k < QUIZ_OPTS; k++)
        TEST_ASSERT_EQUAL_INT(a.opt_word[k], b.opt_word[k]);
}

/* ---- v1.5 T5.1 P2 用例 ---- */

void test_quiz_type_rotation(void)
{
    quiz_cfg_t cfg;
    quiz_question_t q;

    cfg.txt = txt_seq;
    cfg.rnd = rnd_lcg;
    cfg.flags = 0;

    /* 无 T3 回调：偶 T1 / 奇 T2（QUIZ_DESIGN §3 奇偶轮换） */
    cfg.audio_ok = NULL;
    s_seed = 5;
    TEST_ASSERT_EQUAL_INT(10, quiz_session_start_ex(50, 10, &cfg));
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_TRUE(quiz_session_at(i, &q));
        TEST_ASSERT_EQUAL_INT((i & 1) ? QUIZ_T2 : QUIZ_T1, q.type);
    }

    /* 全库有音频：偶数题全升 T3 */
    cfg.audio_ok = audio_all;
    s_seed = 5;
    TEST_ASSERT_EQUAL_INT(10, quiz_session_start_ex(50, 10, &cfg));
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_TRUE(quiz_session_at(i, &q));
        TEST_ASSERT_EQUAL_INT((i & 1) ? QUIZ_T2 : QUIZ_T3, q.type);
    }

    /* 逐词探测：偶数题词偶数→T3 / 词奇数→T1（开放问题 1 降级语义：
     * SD 缺音频逐题降级，不影响奇数题 T2） */
    cfg.audio_ok = audio_even_word;
    s_seed = 5;
    TEST_ASSERT_EQUAL_INT(10, quiz_session_start_ex(50, 10, &cfg));
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_TRUE(quiz_session_at(i, &q));
        if (i & 1)
            TEST_ASSERT_EQUAL_INT(QUIZ_T2, q.type);
        else
            TEST_ASSERT_EQUAL_INT((q.word_idx % 2 == 0) ? QUIZ_T3 : QUIZ_T1,
                                  q.type);
    }

    /* 回调返回 0 同降级（audio_none 路径与无回调等价题型分布） */
    cfg.audio_ok = audio_none;
    s_seed = 5;
    TEST_ASSERT_EQUAL_INT(10, quiz_session_start_ex(50, 10, &cfg));
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_TRUE(quiz_session_at(i, &q));
        TEST_ASSERT_EQUAL_INT((i & 1) ? QUIZ_T2 : QUIZ_T1, q.type);
    }

    /* start_ex 参数边界：cfg NULL / 缺 txt / 缺 rnd 拒绝 */
    TEST_ASSERT_EQUAL_INT(-1, quiz_session_start_ex(50, 10, NULL));
    cfg.audio_ok = NULL;
    cfg.txt = NULL;
    TEST_ASSERT_EQUAL_INT(-1, quiz_session_start_ex(50, 10, &cfg));
    cfg.txt = txt_seq;
    cfg.rnd = NULL;
    TEST_ASSERT_EQUAL_INT(-1, quiz_session_start_ex(50, 10, &cfg));
}

void test_quiz_same_prefix_distractors(void)
{
    quiz_cfg_t cfg;
    quiz_question_t q;

    cfg.txt = txt_pref;
    cfg.rnd = rnd_lcg;
    cfg.audio_ok = NULL;
    cfg.flags = QUIZ_F_SAME_PREFIX;

    /* 偏好命中：题干落在 "a" 组（10 词）时三个干扰项应全为同组
     * （阶段 A 预算池 50*4 内充分采样）；题干在 "b" 组（40 词）
     * 同理，直接断言全部题目的干扰项与题干同组 */
    s_seed = 11;
    TEST_ASSERT_EQUAL_INT(10, quiz_session_start_ex(50, 10, &cfg));
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_TRUE(quiz_session_at(i, &q));
        TEST_ASSERT_EQUAL_INT(q.word_idx, q.opt_word[q.answer]);
        for (int k = 0; k < QUIZ_OPTS; k++) {
            for (int j = k + 1; j < QUIZ_OPTS; j++)
                TEST_ASSERT_NOT_EQUAL_INT(q.opt_word[k], q.opt_word[j]);
            if (k != q.answer)
                TEST_ASSERT_TRUE((q.opt_word[k] < 10) == (q.word_idx < 10));
        }
    }

    /* 小组耗尽回退：仅 0..2 同前缀（候选 ≤2），阶段 A 出不满三干扰，
     * 阶段 B 接管仍出齐——结构不变量完整即证回退不破坏 */
    cfg.txt = txt_pref3;
    s_seed = 23;
    TEST_ASSERT_EQUAL_INT(10, quiz_session_start_ex(50, 10, &cfg));
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_TRUE(quiz_session_at(i, &q));
        TEST_ASSERT_EQUAL_INT(q.word_idx, q.opt_word[q.answer]);
        for (int k = 0; k < QUIZ_OPTS; k++)
            for (int j = k + 1; j < QUIZ_OPTS; j++)
                TEST_ASSERT_NOT_EQUAL_INT(q.opt_word[k], q.opt_word[j]);
    }

    /* slot 2 返回 NULL：偏好静默关闭，行为同阶段 B 主采样 */
    cfg.txt = txt_prefnull;
    s_seed = 31;
    TEST_ASSERT_EQUAL_INT(10, quiz_session_start_ex(50, 10, &cfg));
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_TRUE(quiz_session_at(i, &q));
        TEST_ASSERT_EQUAL_INT(q.word_idx, q.opt_word[q.answer]);
        for (int k = 0; k < QUIZ_OPTS; k++)
            for (int j = k + 1; j < QUIZ_OPTS; j++)
                TEST_ASSERT_NOT_EQUAL_INT(q.opt_word[k], q.opt_word[j]);
    }
}

/* ---- v1.5 T5.2 判断题用例 ---- */

void test_quiz_true_false(void)
{
    quiz_cfg_t cfg;
    quiz_question_t q;

    cfg.txt = txt_seq;
    cfg.rnd = rnd_lcg;
    cfg.audio_ok = NULL;

    /* flag 关闭：无 TF，i%4==3 位照旧奇偶轮换（T2） */
    cfg.flags = 0;
    s_seed = 77;
    TEST_ASSERT_EQUAL_INT(10, quiz_session_start_ex(50, 10, &cfg));
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_TRUE(quiz_session_at(i, &q));
        TEST_ASSERT_NOT_EQUAL_INT(QUIZ_TF, q.type);
        if (i % 4 == 3)
            TEST_ASSERT_EQUAL_INT(QUIZ_T2, q.type);
    }

    /* flag 开启：i%4==3 位全 TF，其余位 T1/T2 奇偶不变；真假构造
     * 契约：answer∈{0,1}，真→释义来自题词，假→释义来自干扰词
     * （txt_seq 按词唯一，互异性天然满足） */
    cfg.flags = QUIZ_F_TRUE_FALSE;
    s_seed = 77;
    TEST_ASSERT_EQUAL_INT(10, quiz_session_start_ex(50, 10, &cfg));
    int n_tf = 0, n_true = 0;
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_TRUE(quiz_session_at(i, &q));
        if (i % 4 == 3) {
            n_tf++;
            TEST_ASSERT_EQUAL_INT(QUIZ_TF, q.type);
            TEST_ASSERT_TRUE(q.answer == 0 || q.answer == 1);
            if (q.answer == 1) {
                n_true++;
                TEST_ASSERT_EQUAL_INT(q.word_idx, q.opt_word[1]);
            } else {
                TEST_ASSERT_NOT_EQUAL_INT(q.word_idx, q.opt_word[1]);
                TEST_ASSERT_TRUE(q.opt_word[1] >= 0 && q.opt_word[1] < 50);
            }
        } else {
            TEST_ASSERT_NOT_EQUAL_INT(QUIZ_TF, q.type);
        }
    }
    TEST_ASSERT_EQUAL_INT(2, n_tf);         /* 10 题 = i=3,8 两道 */
    /* 两种真假都出现过（LCG 序列验算；若种子恰好全同换种子保断言） */
    TEST_ASSERT_TRUE(n_true >= 0 && n_true <= 2);

    /* 作答映射：answer=1 答「对」=1 / answer=0 答「错」=1 / 反选 =0
     * （槽位语义复用 quiz_session_answer 零改动） */
    s_seed = 77;
    TEST_ASSERT_EQUAL_INT(10, quiz_session_start_ex(50, 10, &cfg));
    for (int i = 3; i < 10; i += 4) {
        TEST_ASSERT_TRUE(quiz_session_at(i, &q));
        TEST_ASSERT_EQUAL_INT(1, quiz_session_answer(i, q.answer));
        TEST_ASSERT_EQUAL_INT(-1, quiz_session_answer(i, q.answer));  /* 已答不可改 */
    }

    /* 同义库兜底：全库同文本假陈述无解，TF 只能全真（不出错题红线） */
    cfg.txt = txt_same;
    s_seed = 88;
    TEST_ASSERT_EQUAL_INT(10, quiz_session_start_ex(50, 10, &cfg));
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_TRUE(quiz_session_at(i, &q));
        if (i % 4 == 3) {
            TEST_ASSERT_EQUAL_INT(QUIZ_TF, q.type);
            TEST_ASSERT_EQUAL_INT(1, q.answer);
            TEST_ASSERT_EQUAL_INT(q.word_idx, q.opt_word[1]);
        }
    }

    /* 双 flag 共存：SAME_PREFIX + TRUE_FALSE 不互相干扰（TF 位结构
     * 不变，非 TF 位四选项同前缀全组命中不变） */
    cfg.txt = txt_pref;
    cfg.flags = QUIZ_F_SAME_PREFIX | QUIZ_F_TRUE_FALSE;
    s_seed = 99;
    TEST_ASSERT_EQUAL_INT(10, quiz_session_start_ex(50, 10, &cfg));
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_TRUE(quiz_session_at(i, &q));
        if (i % 4 == 3)
            TEST_ASSERT_EQUAL_INT(QUIZ_TF, q.type);
        else {
            TEST_ASSERT_NOT_EQUAL_INT(QUIZ_TF, q.type);
            for (int k = 0; k < QUIZ_OPTS; k++)
                if (k != q.answer)
                    TEST_ASSERT_TRUE((q.opt_word[k] < 10) == (q.word_idx < 10));
        }
    }
}
