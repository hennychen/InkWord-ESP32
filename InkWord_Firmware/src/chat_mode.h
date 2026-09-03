/**
 * @file chat_mode.h
 * @brief AI 语音对话模式状态机 (P2B，2026-08-24；A1 模式参数化 2026-08-28)
 *
 * AI 后端化红线：设备只录音上传 / 下载播放，ASR+LLM+TTS 全在后端
 * （POST /api/device/chat 单端点闭环，协议冻结见 docs/AI_CHAT_MODE.md
 * §7 模式扩展：?mode={free|scenario|translate}&scenarioId={code}）。
 * 五态循环 idle → recording(≤10s，VAD 断或中键说完即发) → uploading
 * → thinking(下载回复 MP3) → playing → idle；语音优先、屏幕克制：
 * 环路内仅状态区局刷（ui_render_chat），三色面板（无局刷能力）
 * 零渲染纯语音+震动。
 *
 * 按键：中=开始/发送/打断重说，SET=收藏本轮生词（A3：idle 态触发，
 * 任务上下文逐条推 /sync/collect），长按中或 RST=请求退出（T2.2 栈化
 * 后退出编排内聚在 g_chat_page.on_button：haptic + study_mode_exit_chat
 * + pop_if + render_top）；haptic：录音起一短震、回复到两短震、
 * 网络失败一长震（回 idle 不退模式）。电源零改动：每次按键
 * power_note_activity 自然续期。
 */
#ifndef INKWORD_CHAT_MODE_H
#define INKWORD_CHAT_MODE_H

#include <stdbool.h>

#include "button_handler.h"   /* nav_key_t / button_event_t */
#include "page_router.h"      /* T2.2 栈化：page_t */

#ifdef __cplusplus
extern "C" {
#endif

/** 对话状态（ui_render_chat 屏显依据） */
typedef enum {
    CHAT_STATE_IDLE = 0,      /**< 待录音（中键开始；屏驻留末句回复） */
    CHAT_STATE_RECORDING,     /**< 录音中（VAD 自动断 / 中键说完即发） */
    CHAT_STATE_UPLOADING,     /**< 上传 WAV 等后端 ASR+LLM+TTS */
    CHAT_STATE_THINKING,      /**< 下载回复 MP3 */
    CHAT_STATE_PLAYING,       /**< 播放回复（中键打断重说） */
    CHAT_STATE_NETFAIL,       /**< 网络不可用（短暂提示后回 idle） */
} chat_state_t;

/** 末句回复缓冲上限（后端 EnforceLimits ≤260 字符 + 余量） */
#define CHAT_REPLY_MAX (256)

/** 场景预热缓冲上限（后端 WarmupIntro ≤180 UTF-8 字节 + 余量） */
#define CHAT_WARMUP_MAX (192)

/** A3 生词命中：后端 wordHits 上限（ChatService.MaxWordHits 对齐） */
#define CHAT_HIT_MAX       (5)
/** 词条文本上限（英文词条余量） */
#define CHAT_HIT_TEXT_MAX  (24)
/** 云端词条 Guid（36 字符 + NUL） */
#define CHAT_HIT_CLOUD_MAX (40)

/**
 * @brief 对话模式请求（A1）：后端 query 参数 + 屏显标题。
 *
 * mode 空串 = free（URL 不携 query，与老固件请求逐字节一致）；
 * scenario 仅 mode="scenario" 时非空；title 为屏显标签（如
 * 「餐厅点餐」/「英→中翻译」，free 缺省 "AI Chat"）。
 */
typedef struct {
    char mode[12];      /* "" | "free" | "scenario" | "translate" */
    char scenario[16];  /* 场景 Id 短码（food/directions/...） */
    char title[24];     /* ui_render_chat 标题（UTF-8 CJK ≤7 字 + NUL） */
} chat_request_t;

/**
 * @brief 进入对话模式（快捷菜单项触发；study_mode_enter_chat 预检
 *        通过后调用）：复位状态、保存模式请求并启动常驻对话任务
 *        （6KB 栈，模式生命周期内轮询触发位驱动轮次，不阻塞按键
 *        回调）。
 */
void chat_mode_enter(const chat_request_t *req);

/**
 * @brief 请求退出：置取消位 + 停播 + 清模式标志；常驻任务在最近的
 *        循环边界静默收尾（HTTP 阻塞期最长 45s 后自然退出，渲染
 *        调用全部跳过；chat_tmp.mp3 由任务退出路径删除，深睡重启
 *        后残留文件由下一轮下载覆盖，无害）。
 */
void chat_mode_request_exit(void);

/**
 * @brief 模式内按键统一入口（main.on_button 在 MODE_CHAT 时全量转发）。
 * @return true 按键已消费；false 请求退出（长按中 / RST，调用方执行
 *         study_mode_exit_chat + 恢复渲染）。
 */
bool chat_mode_on_button(nav_key_t id, button_event_t event);

/** 模式是否激活（任务生命周期域）。 */
bool chat_mode_is_active(void);

/** 页面路由实例（T2.2 栈化）：render=NULL 自管局刷（环路内
 *  ui_render_chat 局刷，协议先例）；enter=首帧全刷（状态栏+内容区）；
 *  on_button=false 请求退出时内部完成退出编排（haptic+exit_chat+
 *  pop+render_top） */
extern const page_t g_chat_page;

/** 当前状态（g_chat_page.enter 首帧取用）。 */
chat_state_t chat_mode_state(void);

/** 末句回复文本（空串=尚无回合；屏显驻留用）。 */
const char *chat_mode_reply(void);

/** 全句拼接回复（IDLE 回看：空格连接整轮句文本，老路径即整包回复）。 */
const char *chat_mode_full_reply(void);

/** 当前播放句号（1 基；0=未开播，PLAYING 进度指示）。 */
int chat_mode_sentence_no(void);

/** 本轮 ASR 识别文本（meta.transcript 回显）：meta 未到返回 NULL，
 *  到则返回串（空串=后端判无话音）——THINKING 态「你说：…」屏显源 */
const char *chat_mode_heard(void);

/** 本轮录音时长 ms（UPLOADING 态「已录 X.X 秒」屏显源；0=未知） */
int chat_mode_rec_ms(void);

/** 当前场景预热文案（空串=非场景/非首轮；首轮 speaking 态屏显） */
const char *chat_mode_warmup(void);

/** 模式标签（chat_request_t.title，缺省 "AI Chat"；首帧/IDLE 态标题） */
const char *chat_mode_title(void);

/** 本轮生词命中数（0~CHAT_HIT_MAX；新一轮起清零，SET 收藏成功后清零） */
int chat_mode_wordhit_count(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_CHAT_MODE_H */
