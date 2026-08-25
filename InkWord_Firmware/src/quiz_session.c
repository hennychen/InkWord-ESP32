/**
 * @file quiz_session.c
 * @brief 四选一测验出题核心实现（v1.2 T2.1 / QUIZ_DESIGN P1-a；
 *        v1.5 T5.1 P2 题型轮换 + 同前缀干扰）
 *
 * 状态全静态（题表 32 × 28B + 作答 32B），无堆无平台调用；
 * RNG / 文本 / T3 探测全注入，native-test 与固件同一二进制逻辑。
 */
#include "quiz_session.h"

#include <string.h>

#define QUIZ_MIN_POOL    8    /* 词池下限：干扰项来源（开放问题 3 定案） */
#define QUIZ_MAX_ROUNDS  32   /* 静态题表上限 */

static bool            s_active = false;
static quiz_text_fn    s_txt    = NULL;
static quiz_audio_fn   s_audio_ok = NULL;   /* T3 探测（NULL=禁用） */
static uint8_t         s_flags  = 0;
static int             s_rounds = 0;
static quiz_question_t s_q[QUIZ_MAX_ROUNDS];
static int8_t          s_pick[QUIZ_MAX_ROUNDS];   /* 已作答槽位；-1 未答（含跳过） */

/* 前缀安全取用：slot 2 允许回调静态单缓冲，但题干前缀须活到后续
 * 候选查询之后——拷入调用方栈缓冲（≤7B，首 UTF-8 码点 ≤4B 足够，
 * 超长回调截断仅致比较不命中，无 UB） */
static void prefix_copy(char *dst, size_t cap, int idx)
{
    dst[0] = '\0';
    const char *p = s_txt ? s_txt(idx, 2) : NULL;
    if (!p) return;
    size_t n = 0;
    while (p[n] && n < cap - 1) { dst[n] = p[n]; n++; }
    dst[n] = '\0';
}

static bool same_prefix(const char *pp, int cand)
{
    char cp[8];
    prefix_copy(cp, sizeof(cp), cand);
    return pp[0] && strcmp(cp, pp) == 0;
}

/* 选项文本（slot 1）：NULL 防御为空串，比较语义一致 */
static const char *opt_text(int idx)
{
    const char *t = s_txt ? s_txt(idx, 1) : NULL;
    return t ? t : "";
}

static bool same_opt_text(int a, int b)
{
    return strcmp(opt_text(a), opt_text(b)) == 0;
}

/* idx 是否已是本轮题干（第 i 题之前已抽的） */
static bool in_round(int i, int idx)
{
    for (int k = 0; k < i; k++)
        if (s_q[k].word_idx == idx) return true;
    return false;
}

int quiz_session_start(int pool_words, int rounds,
                       quiz_text_fn txt, quiz_rand_fn rnd)
{
    /* P1 兼容包装：无 T3 / 无同前缀（P1 测试零改动） */
    quiz_cfg_t cfg;
    cfg.txt = txt;
    cfg.rnd = rnd;
    cfg.audio_ok = NULL;
    cfg.flags = 0;
    return quiz_session_start_ex(pool_words, rounds, &cfg);
}

int quiz_session_start_ex(int pool_words, int rounds,
                          const quiz_cfg_t *cfg)
{
    s_active = false;
    if (!cfg || !cfg->txt || !cfg->rnd) return -1;
    if (pool_words < QUIZ_MIN_POOL) return -1;
    if (rounds < 1) return -1;
    if (rounds > QUIZ_MAX_ROUNDS) rounds = QUIZ_MAX_ROUNDS;
    if (rounds > pool_words) rounds = pool_words;   /* 不放回抽满即止 */

    s_txt = cfg->txt;
    s_audio_ok = cfg->audio_ok;
    s_flags = cfg->flags;
    for (int i = 0; i < rounds; i++) {
        /* 题干：不放回抽样（查重重抽；rounds ≤ 32 期望开销可忽略） */
        int idx;
        do {
            idx = (int)cfg->rnd((uint32_t)pool_words);
        } while (in_round(i, idx));
        s_q[i].word_idx = idx;

        /* 题型（v1.5 T5.1，QUIZ_DESIGN §3 奇偶轮换）：偶数题探
         * T3（无音频降级 T1），奇数题 T2；T1/T3 同向（词→义，
         * 仅感官通道不同），轮换保持方向交替防同型疲劳。
         * T5.2 判断题：i%4==3 槽位插入（10 题轮约 2 道），陈述构造
         * 与作答均不走四选项路径（槽位语义复用：answer 0=错/1=对，
         * opt_word[1]=释义来源词；opt_word[0]/[2]/[3] 不读，静态
         * 数组残留无害） */
        if ((s_flags & QUIZ_F_TRUE_FALSE) && (i % 4) == 3) {
            s_q[i].type = QUIZ_TF;
            int pos = (int)cfg->rnd(2);          /* 1=陈述真 0=假 */
            s_q[i].opt_word[0] = idx;
            if (pos == 1) {
                s_q[i].opt_word[1] = idx;        /* 真陈述：释义来自题词 */
            } else {                            /* 假陈述：干扰词释义
                                                 * 须与题词释义互异 */
                int cand = idx, budget = pool_words * 4;
                while (budget-- > 0) {
                    int c = (int)cfg->rnd((uint32_t)pool_words);
                    if (c != idx && !same_opt_text(c, idx)) { cand = c; break; }
                }
                if (cand != idx)
                    s_q[i].opt_word[1] = cand;
                else {
                    /* 同义库兜底：释义互异无解，陈述只能为真
                     * （保正确性齐套，不论难度） */
                    pos = 1;
                    s_q[i].opt_word[1] = idx;
                }
            }
            s_q[i].answer = pos;
            s_pick[i] = -1;
            continue;
        }
        s_q[i].type = (uint8_t)((i & 1) ? QUIZ_T2 : QUIZ_T1);
        if (!(i & 1) && s_audio_ok && s_audio_ok(idx) > 0)
            s_q[i].type = QUIZ_T3;

        /* 干扰项三阶段（v1.5 T5.1）：A 同前缀 + 文本去重（预算内
         * 优先，提难度；无前缀语义则跳过）→ B 全库随机采样排除
         * 正确项/本轮题干 + 选项文本（slot 1）两两去重（P1 主采样）
         * → C 兜底仅索引去重（题干词可复用，同义库降级不崩溃） */
        int opts[QUIZ_OPTS];
        opts[0] = idx;
        int n = 1;

        char pp[8];
        if (s_flags & QUIZ_F_SAME_PREFIX)
            prefix_copy(pp, sizeof(pp), idx);
        else
            pp[0] = '\0';

        if (pp[0]) {                          /* 阶段 A：仅同前缀 */
            int budget = pool_words * 4;
            while (n < QUIZ_OPTS && budget-- > 0) {
                int cand = (int)cfg->rnd((uint32_t)pool_words);
                if (cand == idx || in_round(i, cand)) continue;
                if (!same_prefix(pp, cand)) continue;
                bool dup = false;
                for (int k = 0; k < n && !dup; k++)
                    if (opts[k] == cand || same_opt_text(cand, opts[k]))
                        dup = true;
                if (!dup) opts[n++] = cand;
            }
        }
        int budget = pool_words * 4;          /* 阶段 B：文本去重 */
        while (n < QUIZ_OPTS && budget-- > 0) {
            int cand = (int)cfg->rnd((uint32_t)pool_words);
            if (cand == idx || in_round(i, cand)) continue;
            bool dup = false;
            for (int k = 0; k < n && !dup; k++)
                if (opts[k] == cand || same_opt_text(cand, opts[k])) dup = true;
            if (!dup) opts[n++] = cand;
        }
        while (n < QUIZ_OPTS) {   /* 阶段 C 兜底：仅索引去重（题干词可复用） */
            int cand = (int)cfg->rnd((uint32_t)pool_words);
            if (cand == idx) continue;
            bool dup = false;
            for (int k = 0; k < n && !dup; k++)
                if (opts[k] == cand) dup = true;
            if (!dup) opts[n++] = cand;
        }

        /* 正确项插入随机槽位（RNG 注入，分布不受排序偏置） */
        int pos = (int)cfg->rnd(QUIZ_OPTS);
        s_q[i].answer = pos;
        for (int k = 0; k < QUIZ_OPTS; k++)
            s_q[i].opt_word[(pos + k) % QUIZ_OPTS] = opts[k];

        s_pick[i] = -1;
    }
    s_rounds = rounds;
    s_active = true;
    return rounds;
}

bool quiz_session_at(int i, quiz_question_t *out)
{
    if (!s_active || !out || i < 0 || i >= s_rounds) return false;
    *out = s_q[i];
    return true;
}

int quiz_session_answer(int i, int sel)
{
    if (!s_active || i < 0 || i >= s_rounds) return -1;
    if (sel < 0 || sel >= QUIZ_OPTS) return -1;
    if (s_pick[i] >= 0) return -1;        /* 一题一答不可改（不回看） */
    s_pick[i] = (int8_t)sel;
    return sel == s_q[i].answer ? 1 : 0;  /* 对 1 / 错 0；评分由调用方执行 */
}
