/**
 * @file study_mode_machine.h
 * @brief 学习模式状态机 (Task F-16；P1 增错词本)
 *
 * 模式：闪卡(FLASH) / 听写(DICTATION) / 复习(REVIEW) / 阅读(READER) /
 * 错词本(WRONGBOOK) / 收藏浏览(COLLECTION) / 墨封录(MASTERED)。
 * 语义动作由五向导航键映射：
 * 上下=翻词/翻页，中=发音，SET=揭晓/确认，RST=回第一条；长按下=循环
 * 切换模式（错词本/收藏浏览/墨封录为临时视图不入循环，分别由 RST
 * 长按 / 快捷菜单进出，见 V2.1 交互总表）。
 * 栈串联（2026-09-08）：临时视图/功能页均入页栈（page_router），各
 * exit_* 仅归位模式态（FLASH），屏显落点由页栈 pop 决定（回上级：
 * 菜单/学习页/待机页，「从哪进退哪」）。
 * 阅读模式（P3）下游标=页码，序列长度=总页数（reader_engine）；
 * 进入时自动恢复上次阅读页，左/右短按切字号（保持阅读位置）。
 * 序列抽象：闪卡/听写换未墨封过滤视图（2026-09-04 墨封：默认全库
 * 直映射退役——已墨封词从学习主链路移除，learning_state active 视图
 * O(N) 虚游走，wrong/collected/due 同构）；错词本/收藏/墨封录/复习
 * 各自换对应过滤视图，游标与取词均经 seq 接口。
 */
#ifndef INKWORD_STUDY_MODE_MACHINE_H
#define INKWORD_STUDY_MODE_MACHINE_H

#include <stdint.h>
#include <stdbool.h>

#include "chat_mode.h"      /* A1：chat_request_t（enter_chat 模式透传） */
#include "page_router.h"    /* page_t（错词本/收藏/墨封录栈页声明） */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 学习视图栈页（架构拆分 2026-09-17，定义迁 study_mode_machine.c）：
 * 错词本/收藏/墨封录临时视图 page_t 全局，供 main.cpp shortcut_exec /
 * word_view_on_button 及 menu_ui.c act_* 入栈引用 ---- */
extern const page_t g_wrongbook_page;
extern const page_t g_collection_page;
extern const page_t g_mastered_page;
extern const page_t g_dictation_summary_page;  /* R2.2 听写汇总栈页 */
extern const page_t g_practice_summary_page;   /* R4.2 练习完成汇总栈页 */

typedef enum {
    MODE_FLASH = 0,      /**< 闪卡：看词猜义 */
    MODE_DICTATION,      /**< 听写：听音拼写 */
    MODE_REVIEW,         /**< 复习：SRS 到期词 */
    MODE_READER,         /**< 阅读：整本书分页阅读（P3；游标=页码，序列=页序列） */
    MODE_WRONGBOOK,      /**< 错词本：连错词专项（RST 长按进出，不入切换循环） */
    MODE_COLLECTION,     /**< 收藏浏览：收藏词临时视图（快捷菜单进入，不入循环
                              不 NVS 恢复；枚举值固定 5，last_mode 兼容） */
    MODE_CHAT,           /**< AI 对话：语音对话临时视图（P2B，快捷菜单进入，
                              同临时视图纪律：不入循环/不 NVS 恢复/不走
                              apply_mode；生命周期由 chat_mode 任务自理） */
    MODE_QUIZ,           /**< 快速测验：四选一临时视图（v1.2 T2.2，快捷菜单
                              进入；同临时视图纪律第四先例：不入循环/不 NVS
                              恢复/不走 apply_mode；出题核心 quiz_session
                              纯 C（T2.1），题池构造/渲染/作答编排留
                              main.cpp 适配层，QUIZ_DESIGN §6/§7） */
    MODE_BROWSE,         /**< 教材目录浏览：年级→单元→词表三级临时视图
                              （2026-08-28 目录浏览+语音查词设计；第五临时
                              视图先例：不入循环/不 NVS 恢复/不走 apply_mode；
                              枚举值固定 8；选词经 study_mode_seek 定位） */
    MODE_VOICE,          /**< 语音查词：录音→ASR→候选跳词临时视图（同设计；
                              枚举值固定 9，同第五先例；状态机 voice_search.c
                              按键驱动无任务，main.cpp 分发） */
    MODE_MASTERED,       /**< 墨封录：已墨封词临时视图（2026-09-04 墨封功能，
                              第六临时视图先例：不入循环/不 NVS 恢复/不走
                              apply_mode，完整镜像 MODE_COLLECTION；枚举值
                              固定 10；SET 长按=启封移出序列（main.cpp
                              守卫），menu_ui「墨封录」进出） */
    MODE_COUNT
} study_mode_t;

/**
 * @brief 初始化状态机，默认进入闪卡模式。
 */
void study_mode_init(void);

/**
 * @brief 获取当前模式。
 */
study_mode_t study_mode_current(void);

/**
 * @brief 循环切换到下一个模式。
 * @return 切换后的模式。
 */
study_mode_t study_mode_switch_next(void);

/**
 * @brief 直接设置模式（游标归零/READER 进度恢复/遮蔽复位/持久化，
 *        与长按下循环切换副作用一致；临时视图（错词本/收藏/对话）拒绝）。
 */
void study_mode_set(study_mode_t mode);

/**
 * @brief 获取当前模式的显示名称（用于 UI）。
 */
const char *study_mode_name(study_mode_t mode);

/**
 * @brief 在当前模式下处理“上一条/下一条/确认/发音”等语义动作。
 *        由按键事件经模式映射后调用。
 * @param action 0=prev 1=next 2=confirm(闪卡翻义/听写提交) 3=speak
 */
void study_mode_handle_action(int action);

/**
 * @brief 释义当前是否显示（默认 true；SET 翻义切换遮蔽/揭晓自测）。
 *        翻页/切模式后自动回到显示状态。
 */
bool study_mode_is_revealed(void);

/**
 * @brief 光标重置到当前模式第一条并重绘（RST 侧键）。
 */
void study_mode_reset_cursor(void);

/**
 * @brief 进入错词本（RST 长按；临时视图，不持久化 last_mode）。
 * @return true 成功；false 无错词（调用方提示边界反馈）。
 */
bool study_mode_enter_wrongbook(void);

/**
 * @brief 退出错词本回闪卡模式（错词本内 RST 长按）。
 */
void study_mode_exit_wrongbook(void);

/* ---- R4.2 错词练习队列（2026-09-20） ---- */

/**
 * @brief 启动错词练习（错词本内上键长按）：收集错词到队列并洗牌。
 * @return true 成功；false 无错词。
 */
bool study_mode_start_practice(void);

/**
 * @brief 停止练习（练习中上键长按或汇总页退出）。
 */
void study_mode_stop_practice(void);

/**
 * @brief 练习是否激活（进行中或已完成待确认）。
 */
bool study_mode_practice_is_active(void);

/**
 * @brief 已完成练习的词数。
 */
int study_mode_practice_done_count(void);

/**
 * @brief 练习总词数（启动时确定）。
 */
int study_mode_practice_total(void);

/* ---- 收藏浏览临时视图（P 快捷菜单，错词本同构） ---- */

/**
 * @brief 进入收藏浏览（快捷菜单「收藏列表」项；临时视图，不持久化）。
 * @return true 成功；false 空收藏（调用方提示边界反馈）。
 */
bool study_mode_enter_collection(void);

/**
 * @brief 退出收藏浏览回闪卡模式（收藏视图内 RST 长按）。
 */
void study_mode_exit_collection(void);

/* ---- 墨封录临时视图（2026-09-04 墨封功能，收藏浏览同构） ---- */

/**
 * @brief 进入墨封录（快捷菜单「墨封录」项；临时视图，不持久化）。
 * @return true 成功；false 空墨封（调用方提示边界反馈）。
 */
bool study_mode_enter_mastered(void);

/**
 * @brief 退出墨封录回闪卡模式（墨封录内 RST 长按；清空启封自动退同）。
 */
void study_mode_exit_mastered(void);

/* ---- AI 对话临时视图（P2B，chat_mode 五态状态机） ---- */

/**
 * @brief 进入 AI 对话（快捷菜单「AI 对话」二级页确认项；临时视图，
 *        不持久化）。A1 模式参数化：req 携带 mode（free/scenario/
 *        translate）+ scenarioId + 屏显标题，透传 chat_mode_enter。
 *        前置：Wi-Fi 已连接 + 设备 Key 已配 + SD 在位（回复 MP3 落盘），
 *        任一不满足返回 false 由调用方给边界反馈。
 * @param req 对话请求（mode/scenario/title；NULL=free 缺省）。
 * @return true 成功；false 前置不满足（未进入，无副作用）。
 */
/** 进入 AI 对话（前置预检：Wi-Fi/设备 Key/SD）。
 *  @return 0=成功；1=无网络；2=设备未注册；3=无 SD 卡（菜单层据此留页提示） */
int study_mode_enter_chat(const chat_request_t *req);

/**
 * @brief 退出 AI 对话回闪卡模式（模式内长按中 / RST）：停对话任务、
 *         删临时音频，不写 last_mode（临时视图纪律）。
 */
void study_mode_exit_chat(void);

/* ---- 快速测验临时视图（v1.2 T2.2，quiz_session 出题核心） ---- */

/**
 * @brief 进入快速测验（快捷菜单「快速测验」项；临时视图，不持久化）。
 *        前置：词库 ≥ 8 词（quiz_session 干扰项来源下限，QUIZ_DESIGN
 *        开放问题 3）；不满足返回 false 由调用方给边界反馈。
 *        题池构造、quiz_session_start 与首帧渲染由调用方在 enter
 *        成功后执行（QUIZ_DESIGN §7 数据流）。
 * @return true 成功；false 词库不足（未进入，无副作用）。
 */
bool study_mode_enter_quiz(void);

/**
 * @brief 退出快速测验回闪卡模式（模式内 RST / 小结页任意键）。
 *        不写 last_mode（临时视图纪律）；已答题评分即时生效，
 *        退出无补偿动作。
 */
void study_mode_exit_quiz(void);

/* ---- 教材目录浏览临时视图（2026-08-28 设计，第五先例） ---- */

/**
 * @brief 进入教材目录浏览（快捷菜单「教材目录」项；临时视图，不持久化）。
 *        前置：词库 ≥ 1（目录索引由装载链路 catalog_build 构建，空/失配
 *        由渲染层兑底）；三级视图状态由 browse_mode 自理，首帧渲染由
 *        调用方在 enter 成功后执行。
 * @return true 成功；false 词库为空（未进入，无副作用）。
 */
bool study_mode_enter_browse(void);

/**
 * @brief 退出目录浏览回闪卡（视图内 RST 长按/逐级退到顶退出）：
 *        游标恢复进视图前的闪卡位置（浏览取消不丢学习进度）；
 *        选词跳转走 study_mode_seek（模式已切，勿重复调用）。
 */
void study_mode_exit_browse(void);

/* ---- 语音查词临时视图（同设计，chat 前置先例简化版） ---- */

/**
 * @brief 进入语音查词（快捷菜单「语音查词」项；临时视图，不持久化）。
 *        前置：Wi-Fi 已连 + 设备 Key 已配；无 SD 依赖（PSRAM 缓冲
 *        直传，无音频落盘）。状态机复位/首帧由调用方在 enter 成功后执行。
 * @return true 成功；false 前置不满足（未进入，无副作用）。
 */
bool study_mode_enter_voice_search(void);

/**
 * @brief 退出语音查词回闪卡（视图内长按 RST/长按中）：游标恢复进视图
 *        前位置；已选词跳转走 study_mode_seek（模式已切，勿重复调用）。
 */
void study_mode_exit_voice_search(void);

/**
 * @brief 词库索引定位：切 FLASH 模式 + 游标=index（钳位）+ 渲染。
 *        供 browse（目录选词）/ voice_search（候选确认）共用；
 *        不写 NVS（FLASH 本就可恢复）；空词库无操作。
 */
void study_mode_seek(int word_index);

/**
 * @brief 取消收藏（SET 长按 toggle）之后的序列收缩钳位：当前词移出收藏
 *        序列，后词前移；序列清空自动退回闪卡；游标越界钳到 n-1
 *        （取消末词时显示前一词不跳跃，与 after_quality 回绕 0 略异）。
 * @return true 表示游标/模式变化，需重绘当前页。
 */
bool study_mode_after_uncollect(void);

/**
 * @brief 当前显示词的词库索引（评分/收藏的目标词）。
 *        错词本模式下为错词序列游标映射后的词库索引。
 */
int study_mode_current_word_index(void);

/**
 * @brief 当前模式序列位置（1 基显示用：pos+1/total）。
 */
int study_mode_seq_pos(void);

/**
 * @brief 当前模式序列总长（错词本=错词数，其余=词库总数）。
 */
int study_mode_seq_total(void);

/**
 * @brief 阅读模式字号切换（左/右短按）：切级 16/20/24px 循环，重建页表
 *        并按当前页首字符就近重定位游标后重绘。非阅读模式/无书时无操作。
 */
void study_mode_reader_font_step(int dir);

/**
 * @brief 阅读模式章节跳转（上/下长按）：dir=+1 下一章 / -1 上一章。
 *        非阅读模式/无章节时无操作。
 */
void study_mode_reader_chapter_step(int dir);

/**
 * @brief 阅读模式跳转到指定页（目录/书签跳转用）。
 *        直接设置游标 page 并渲染，不写 NVS（下次渲染自动保存）。
 *        非阅读模式/页码越界时无操作。
 */
void study_mode_reader_goto_page(int page);

/**
 * @brief 墨封/启封后的序列收缩钳位（learning_state_toggle_master 之后
 *        调用）：当前词移出所在序列（闪卡/听写=active 视图、错词本=
 *        连错清零、复习=到期过滤、墨封录=启封移出），后词前移、游标
 *        钳 n-1（after_uncollect 同策略）；错词本/墨封录清空自动退回
 *        闪卡；闪卡序列清空钳 0（渲染层显「全部词已墨封」空态页）。
 * @return true 表示游标/模式变化，需重绘当前页。
 */
bool study_mode_after_master(void);

/**
 * @brief 评分应用后的错词本序列维护（learning_state_apply_quality 之后调用）。
 *        仅错词本模式有副作用：quality>=3 的词移出错词序列，后词前移、
 *        游标原位停留（越界回绕）；序列清空自动退回闪卡模式。
 * @return true 表示游标/模式变化，需重绘当前页。
 */
bool study_mode_after_quality(int quality);

/**
 * @brief 复习模式自评后的到期序列收缩（评分即置会话 done 位，后词前移、
 *        游标钳 n-1；序列清空钳 0 由渲染层显空态页）。任一 quality 出队。
 * @return true 表示序列变化，需重绘当前页（仅 MODE_REVIEW 有副作用）。
 */
bool study_mode_after_due_review(void);

/* ---- 跟读评测编排（P1，AI_SPEECH_ASSESSMENT §3.3）---- */

/** 跟读屏显状态（ui_render_pron 三态 + 失败态；分数态 total 为分值，
 *  FAIL 态 total 复用传错误码：-1/-2=评分失败 -3=未听到话音） */
typedef enum {
    PRON_STATE_RECORDING = 0,   /**< 跟读中（Speak now） */
    PRON_STATE_SCORING,         /**< 评分中 */
    PRON_STATE_RESULT,          /**< 结果（分数 + 引擎角标） */
    PRON_STATE_FAIL,            /**< 失败（网络/无声） */
} pron_state_t;

/**
 * @brief 跟读任务是否在跑（录音/上传期间；按键应转发 any_key 取消）。
 */
bool study_mode_pron_active(void);

/**
 * @brief 跟读结果屏是否还亮着（任务已退、等任意键回词卡）。
 */
bool study_mode_pron_ui_visible(void);

/**
 * @brief 跟读期间任意按键统一入口：任务在跑→置取消（任务自恢复词卡）；
 *        结果屏亮着→立即恢复词卡。调用方吞掉本次按键。
 */
void study_mode_pron_any_key(void);

/* ---- 听写会话统计（R2.2，2026-09-20）---- */

/** 听写会话统计 */
typedef struct {
    int total;     /**< 本次会话总词数 */
    int correct;   /**< 答对（Q5） */
    int wrong;     /**< 答错（Q1） */
} dictation_session_t;

/**
 * @brief 重置听写会话统计（进入听写模式时调用）。
 */
void dictation_session_reset(void);

/**
 * @brief 记录听写自评结果（learning_state_apply_quality 之前调用）。
 * @param quality 质量分（1=忘记/错，5=简单/对）
 */
void dictation_session_record(int quality);

/**
 * @brief 获取听写会话统计（只读）。
 */
dictation_session_t dictation_session_get(void);

/**
 * @brief 听写会话是否有数据（total > 0）。
 */
bool dictation_session_has_data(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_STUDY_MODE_MACHINE_H */
