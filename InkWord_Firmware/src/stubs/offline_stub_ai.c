/**
 * @file offline_stub_ai.c
 * @brief AI 轴桩（INKWORD_FEATURE_AI=0，开源通用化 Phase 1）
 *
 * 顶替 chat_mode.c / voice_search.c / chat_ui.c 三个编译单元。
 * 头文件原样保留——include 既有头保证签名严格一致。
 *
 * 桩语义（返回值走既有降级路径，业务文件零改动）：
 *   - study_mode_enter_chat / enter_voice_search 预检在 wifi/sync 桩
 *     作用下自然拒绝（返回非 0 → menu_ui/shortcut 入口长震），
 *     MODE_CHAT/MODE_VOICE 实际不可达——桩仅兜底防御路径（如
 *     main.cpp base_render 的 MODE_CHAT 防御分支）；
 *   - chat_mode_reply 返回 ""（非 NULL，防 UI 解引用）；
 *   - g_chat_page 的 on_button 为返回 false 的真实函数（NULL 会造成
 *     dispatch 吞键死页，同 lan/wifi 桩铁律）。
 *
 * 纪律：被顶替模块新增公开 API 时必须同步本桩（链接错误兜底提醒）。
 */
#include "chat_mode.h"
#include "voice_search.h"
#include "chat_ui.h"
#include "page_router.h"

#include <stddef.h>

/* ---- chat_mode.h ---- */

void chat_mode_enter(const chat_request_t *req) { (void)req; }    /* STUB */
void chat_mode_request_exit(void)               { }              /* STUB */
bool chat_mode_on_button(nav_key_t id, button_event_t event)     /* STUB */
{
    (void)id; (void)event;
    return false;
}
bool chat_mode_is_active(void)    { return false; }              /* STUB：enter_chat 预检失败依据 */
int chat_mode_sentence_no(void)   { return 0; }                  /* STUB */
int chat_mode_rec_ms(void)        { return 0; }                  /* STUB */
int chat_mode_wordhit_count(void) { return 0; }                  /* STUB */
chat_state_t chat_mode_state(void){ return CHAT_STATE_IDLE; }    /* STUB */
const char *chat_mode_reply(void) { return ""; }                 /* STUB：非 NULL 防 UI 解引用 */
const char *chat_mode_full_reply(void) { return ""; }            /* STUB：空串=无内容（头文件语义） */
const char *chat_mode_heard(void)  { return NULL; }               /* STUB：NULL=meta 未到（头文件语义） */
const char *chat_mode_warmup(void) { return ""; }                /* STUB：空串=非场景/非首轮 */
const char *chat_mode_title(void)  { return "AI Chat"; }         /* STUB：free 模式缺省标题 */

/* ---- voice_search.h ---- */

void voice_search_reset(void)         { }                        /* STUB */
void voice_search_request_exit(void)  { }                        /* STUB */
bool voice_search_is_active(void)     { return false; }         /* STUB */
voice_state_t voice_search_state(void) { return VOICE_IDLE; }   /* STUB：闲置态 */
void voice_search_render(void)        { }                        /* STUB：零渲染直接返回（三色屏先例） */
bool voice_search_on_button(nav_key_t id, button_event_t event)  /* STUB */
{
    (void)id; (void)event;
    return false;
}

/* ---- chat_ui.h ---- */

void ui_render_chat(chat_state_t st, const char *text)           /* STUB：main base_render 防御分支 */
{
    (void)st; (void)text;
}
void ui_chat_anim_tick(void)            { }                      /* STUB：无 THINKING 涟漪动画 */

/* ---- 桩 page 实例（误入任意键退出；on_button NULL=吞键死页铁律） ---- */

static void stub_render(void) { }   /* 防御性空渲染：render_top 零开销返回 */

static bool stub_anykey_exit(nav_key_t id, button_event_t event)
{
    (void)id; (void)event;
    return false;
}

const page_t g_chat_page = { "chat", stub_render, stub_anykey_exit, NULL, NULL, false };
