/**
 * @file learning_state.h
 * @brief 本地学习状态层 (P1 错词本 + 收藏；P2 加上报事件队列；
 *        2026-09-04 加墨封 mastered 第四态)
 *
 * 每词 FSRS 状态 + 连错计数 + 收藏标志 + 墨封标志。持久化 sparse
 * 格式（LR04 卡组隔离，LR05 加墨封）：NVS blob 只存非默认态词
 * （学过/连错>0/已收藏/已墨封），键按卡组分派（默认组 "lr_state"、
 * 其余 "lr_st_<id>"），切卡组=旧组保存 + 新组恢复（进度互不丢）；
 * nvs 已扩容 192KB（T1.5，上限 4000）；词库规模变化仍整体作废
 * （保守策略）。保存时机：评分/收藏/墨封仅置脏标记，主循环
 * learning_state_maybe_save() 静默 5s 后落盘（按键路径零 NVS 阻塞，
 * 掉电窗口 ≤5s，与事件队列不持久化策略一致）。
 *
 * 连错维护规则与后端 SrsService.ApplyReview 严格同步：
 *   quality < 3 连错递增（>0 即入错词本），quality >= 3 清零移出；
 *   墨封置位同步清零（用户声明「我认识」等价声明式通过，与后端
 *   DeviceController.SyncMaster 双端镜像）。
 *
 * 墨封语义（Anki suspend 哲学，2026-09-04）：调度层过滤标志——
 * 闪卡/听写序列换未墨封视图、到期/测验题池排除；FSRS 节点与收藏
 * 原样保留（启封即按原到期回队，无损可逆）。
 *
 * 上报队列（P2）：评分/收藏/墨封动作同时入内存环形队列，由 main.cpp
 * background_task 联网时逐条 flush 到 sync_client（词须有 cloudId，
 * 即后端 /admin/words/export 导出的 Guid；本地导入词自动跳过）。
 * 队列不持久化：重启丢失未上报事件，但 NVS 已存最终态，云端仅
 * 少中间事件（连错/收藏/墨封为幂等 set，最终一致）。
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
    int  quality;               /**< 0~5；-1 = 收藏事件；-2 = 墨封事件 */
    bool collected;             /**< 收藏事件=收藏布尔；墨封事件复用承载
                                     mastered 布尔（内存结构，无持久化
                                     兼容负担，sync_session flush 分派） */
} lr_event_t;

/**
 * @brief 初始化：锁定卡组与跟踪词数，并从该卡组 NVS 键恢复状态。
 *        词库词条数变化时旧状态整体作废（保守策略）。
 * @param word_count 词库实际词条数（超出 LEARNING_STATE_MAX 截断）。
 * @param deck_id    卡组短 id（deck_manager_active_id；""=默认卡组）。
 */
void learning_state_init(int word_count, const char *deck_id);

/**
 * @brief 切词书专用重载（LR04 语义，v1.4 T4.2）：旧卡组进度先落盘
 *        （键按旧组分派，切书不丢学习进度），再重锁词数并从新卡组键
 *        恢复其历史状态（无记录=从零学）。与 init 的差异：复用已分配
 *        的状态数组（不重复 malloc），清空待上报事件队列与脏标记
 *        （word_idx 对新卡组无意义）；lr_stats 今日/连续口径保留
 *        （跨卡组学习行为统计，验收项「切书不丢今日进度」）。
 */
void learning_state_reload(int word_count, const char *deck_id);

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

/* ---- 墨封（mastered，2026-09-04 Anki suspend 哲学：调度层过滤，
 * FSRS/收藏原样保留，启封无损回队；master 时连错清零移出错词本） ---- */

/**
 * @brief 墨封/启封切换，返回切换后的状态；置位方向同步清连错
 *        （声明式通过）+ 入上报事件队列（quality=-2）+ 置脏延迟落盘。
 */
bool learning_state_toggle_master(int word_idx);

/**
 * @brief 当前词是否已墨封。
 */
bool learning_state_is_mastered(int word_idx);

/**
 * @brief 墨封数量（墨封录视图 total）。
 */
int learning_state_mastered_count(void);

/**
 * @brief 墨封录视图取词：第 pos 个已墨封词的词库索引，越界 -1。
 */
int learning_state_mastered_at(int pos);

/**
 * @brief 未墨封词数量（闪卡/听写序列 total——学习主链路过滤）。
 */
int learning_state_active_count(void);

/**
 * @brief 未墨封视图取词：第 pos 个未墨封词的词库索引（按索引序），
 *        越界 -1（wrong/collected/due 同构 O(N) 虚游走）。
 */
int learning_state_active_at(int pos);

/**
 * @brief 未学且未墨封的词数（daily_plan_done 可学新词耗尽判据分子）。
 */
int learning_state_active_new_count(void);

/**
 * @brief 错词数量（consecutive_wrong > 0 的词条数）。
 */
int learning_state_wrong_count(void);

/**
 * @brief 错词视图取词：第 pos 个错词的词库索引（按索引序），越界返回 -1。
 */
int learning_state_wrong_at(int pos);

/* ---- 收藏视图（P 快捷菜单：收藏浏览 MODE_COLLECTION，镜像错词本一对） ---- */

/**
 * @brief 收藏数量（collected == true 的词条数）。
 */
int learning_state_collected_count(void);

/**
 * @brief 收藏视图取词：第 pos 个收藏词的词库索引（按索引序），越界返回 -1。
 */
int learning_state_collected_at(int pos);

/* ---- 复习到期视图（2026-08-24 PRD「复习=SRS 到期词」落地，第三例同构） ---- */

/**
 * @brief 到期待复习词数：已学（stability>0）且 srs_is_due，且本会话
 *        未评过分（apply_quality 置会话 done 位——刚学/刚评的词不再
 *        重复推送，重启后重新列出到期词）。
 */
int learning_state_due_count(void);

/**
 * @brief 到期视图取词：第 pos 个到期词的词库索引（按索引序），越界 -1。
 */
int learning_state_due_at(int pos);

/**
 * @brief 词是否未学（无 FSRS 状态 stability==0；quiz 题池新词补足用，
 *        QUIZ_DESIGN §3。注意不含墨封判定——补足侧需叠加
 *        !is_mastered 排除（quiz_ui.c 题池构造）。
 */
bool learning_state_is_new(int word_idx);

/* ---- 今日学习统计（2026-08-24，百词斩「今日进度」借鉴；NVS lr_stats） ---- */

/**
 * @brief 今日首评词数（评分时 stability==0 的首学；全局跨组累计，T4.2 口径）。
 */
int learning_state_today_new(void);

/**
 * @brief 指定卡组今日首评词数（v1.5 T5.5 科目级配额分子；NVS lr_stdeck
 *        SD01 按组小表）。跨日未评分/新组无条目/时钟未同步均返回 0。
 * @param deck_id 卡组短 id（NULL/""=默认组）。
 */
int learning_state_deck_today_new(const char *deck_id);

/**
 * @brief 考试冲刺：到期视图 horizon 放宽（v1.5 T5.5）。
 *        days=0 恢复标准 due 判定；days>0 时未来 days 天内将到期的
 *        词也计入到期视图（日期反推优先清账，main.cpp 按 exam_urgent
 *        注入；切卡组/重启保持，仅外部重设）。无返回，静默钳位 0~99。
 */
void learning_state_set_due_horizon(int days);

/**
 * @brief 今日评分总次数。
 */
int learning_state_today_reviews(void);

/**
 * @brief 连续学习天数（每日 ≥1 次评分连续计数；时钟未同步期间统计挂起）。
 */
int learning_state_streak_days(void);

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

/* ---- 上报事件队列（P2 云端闭环，sync_session background_task 消费） ---- */

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
