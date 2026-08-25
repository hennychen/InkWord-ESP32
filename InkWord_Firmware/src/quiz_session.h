/**
 * @file quiz_session.h
 * @brief 四选一测验出题核心（v1.2 T2.1 / QUIZ_DESIGN P1-a；
 *        v1.5 T5.1 P2 题型轮换 + 同前缀干扰，2026-08-25）
 *
 * 纯 C 零平台依赖（同 srs_engine 先例，进 native-test）；只认词条
 * 索引、文本经回调获取、RNG 注入——泛化约束（ROADMAP v1.1 红线）：
 * 核心不含任何英语字段硬编码，英语词卡由 main.cpp 侧适配
 * （slot 0=题干 text / slot 1=选项 meaning 首行），v1.4 全科目
 * 换 txt 回调即用，核心零改动。
 *
 * 出题算法（QUIZ_DESIGN §3，冻结）：题干不放回抽样；干扰项全库
 * 随机采样并排除本轮题干，选项文本（slot 1）两两去重，重试预算
 * 耗尽后放宽文本去重保底出齐（同义库降级不崩溃）。v1.5 T5.1：
 * 同前缀偏好插在文本去重之前（阶段 A，预算内仅收同前缀候选，
 * 耗尽续走原两阶段），题型 T1/T2 奇偶轮换、T3 逐题探测回调
 * （无音频降级 T1，QUIZ_DESIGN 开放问题 1 定案）。
 *
 * 职责边界：本模块不碰 learning_state——作答映射（对=quality 4 /
 * 错=quality 1 / 跳过不评分）的 apply_quality 由调用方执行；
 * 「到期词优先 + 新词补足」的题池构造同样在调用方（经 word_idx
 * 重映射：核心域索引 → learning_state_due_at 视图序，见 main.cpp
 * 适配层）；T3 的音频路径解析与播放也全在调用方（audio_ok 仅探
 * 测可用性，核心不接触文件系统）。
 */
#ifndef INKWORD_QUIZ_SESSION_H
#define INKWORD_QUIZ_SESSION_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QUIZ_OPTS 4   /**< 四选一固定选项数 */

#define QUIZ_T1 0   /**< 看词选义：题干 slot 0 → 选项 slot 1（视觉） */
#define QUIZ_T2 1   /**< 看义选词：题干 slot 1 → 选项 slot 0（视觉） */
#define QUIZ_T3 2   /**< 听音选义：自动播词音 → 选项 slot 1（听觉） */
#define QUIZ_TF 3   /**< 判断题：陈述「题词=释义」真假（v1.5 T5.2） */

/** 一道题：题干词条索引 + 4 选项词条索引 + 正确项槽位 + 题型 */
typedef struct {
    int word_idx;                  /**< 题干（核心域词条索引） */
    int opt_word[QUIZ_OPTS];       /**< 选项（opt_word[answer] 为正确项） */
    int answer;                    /**< 正确项槽位 ∈ [0, QUIZ_OPTS) */
    uint8_t type;                  /**< 题型 QUIZ_T1/T2/T3（v1.5 T5.1） */
} quiz_question_t;

/**
 * @brief 文本回调：核心域词条索引 → 展示文本。
 * @param word_idx 词条索引（调用方域，含题池重映射）
 * @param slot 0=题干文本 / 1=选项文本（去重比较用） /
 *            2=前缀（题干词 slot 0 文本首个 UTF-8 码点，同首字母
 *            干扰采样用；无需前缀语义时返回 NULL/空串即静默关闭）
 * @return NUL 结尾字符串；NULL 视作空串（防御）。slot 2 允许回调
 *         使用静态单缓冲——核心先拷贝题干前缀再查候选（见
 *         quiz_session.c 阶段 A 注释）。
 */
typedef const char *(*quiz_text_fn)(int word_idx, int slot);

/**
 * @brief 音频可用性回调：该词条是否有可播词音（T3 听音选义）。
 *        NULL = 会话禁用 T3（全部降级 T1/T2 轮换）；返回 >0 有音频 /
 *        0 无（该题降级 T1）。只探测不播放——路径解析与播放全在
 *        调用方（核心不接触文件系统，纯 C 约束）。
 */
typedef int (*quiz_audio_fn)(int word_idx);

/** 同首字母干扰项偏好（v1.5 T5.1，QUIZ_DESIGN §3 ③）：阶段 A 预算内
 *  仅收与题干 slot 2 同前缀的候选，耗尽后续走原采样（不阻断出题） */
#define QUIZ_F_SAME_PREFIX 0x01

/** 判断题（v1.5 T5.2）：每 4 题第 4 题（i%4==3）为判断题（10 题轮
 *  约 2 道）；作答槽位语义复用——sel 0=错 / 1=对，answer=1 陈述真
 *  （释义来自题词）/ 0 陈述假（释义来自文本互异的干扰词）；同义库
 *  假陈述无解时 answer 兜底为真（绝不出错题） */
#define QUIZ_F_TRUE_FALSE 0x02

/**
 * @brief 随机数回调：返回 [0, bound) 均匀随机值。
 *        固件注入 esp_fill_random 播种序列，测试注入确定性 LCG。
 */
typedef uint32_t (*quiz_rand_fn)(uint32_t bound);

/** 会话配置（quiz_session_start_ex，v1.5 T5.1） */
typedef struct {
    quiz_text_fn  txt;       /**< 文本回调（必填） */
    quiz_rand_fn  rnd;       /**< RNG 回调（必填） */
    quiz_audio_fn audio_ok;  /**< T3 探测回调（NULL=禁用 T3） */
    uint8_t       flags;     /**< QUIZ_F_* 位集 */
} quiz_cfg_t;

/**
 * @brief 开启一轮测验会话（覆盖旧会话）。
 * @param pool_words 题池词数（词条索引域 [0, pool_words)）
 * @param rounds    期望题数；钳位 min(rounds, pool_words, 32)
 * @return 实际题数（>0 成功）；-1 参数非法 / 词池 < 8
 *         （干扰项来源下限，QUIZ_DESIGN 开放问题 3）
 */
int quiz_session_start(int pool_words, int rounds,
                       quiz_text_fn txt, quiz_rand_fn rnd);

/**
 * @brief 扩展入口（v1.5 T5.1）：题型轮换 + T3 探测 + 同前缀干扰。
 *        语义同 quiz_session_start，另据 cfg：偶数题探 audio_ok
 *        决定 T3（无音频降级 T1）、奇数题 T2；QUIZ_F_SAME_PREFIX
 *        开启阶段 A 同前缀优先采样。P1 四参入口保留为兼容包装
 *        （audio_ok=NULL / flags=0，P1 测试零改动）。
 */
int quiz_session_start_ex(int pool_words, int rounds,
                          const quiz_cfg_t *cfg);

/**
 * @brief 取第 i 题（i ∈ [0, 实际题数)）；会话外 / 越界返回 false。
 */
bool quiz_session_at(int i, quiz_question_t *out);

/**
 * @brief 作答第 i 题选择 sel 槽位（一题一答，不可改）。
 * @return 1 选对 / 0 选错 / -1 非法（未开会话、越界、槽位超界、已答）；
 *         跳过 = 调用方不调本函数直接换题（词保留到期状态）
 */
int quiz_session_answer(int i, int sel);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_QUIZ_SESSION_H */
