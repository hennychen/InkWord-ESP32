/**
 * @file page_router.c
 * @brief 页面路由实现（T1.4）：覆盖层小栈 + base 委托
 *
 * 栈深上限 4：现役最深串联为 menu→settings 同级替换（深 1），
 * 预留 quiz/browse/voice/chat 等临时视图后续接入的极端叠层。
 * 调用方均在主 loop / 按键回调上下文，无并发（standby/chat_ui/
 * power_manager 的 overlay_active 查询同上下文或原子 bool 读）。
 */
#include "page_router.h"
#include "debug_log.h"    /* LOG_E（栈溢出防御日志） */

#include <stddef.h>        /* NULL */

static const char *TAG = "PAGE_ROUTER";   /* debug_log.h LOG 宏依赖 */

#define PAGE_STACK_MAX 4   /* 覆盖层深度上限（menu→settings 等串联场景） */

static const page_t *s_base = NULL;          /* base 页（study_mode 分流） */
static const page_t *s_stack[PAGE_STACK_MAX];
static int s_top = 0;                        /* 栈内页数 */

void page_router_init(const page_t *base)
{
    s_base = base;
}

void page_router_push(const page_t *p)
{
    if (!p) return;
    if (s_top > 0 && s_stack[s_top - 1] == p) return;   /* 幂等 */
    if (s_top >= PAGE_STACK_MAX) {
        LOG_E("page stack overflow (top=%d)", s_top);
        return;
    }
    s_stack[s_top++] = p;
    if (p->enter) p->enter();
}

const page_t *page_router_pop_if(const page_t *p)
{
    if (s_top == 0 || s_stack[s_top - 1] != p) return NULL;
    s_top--;
    if (p->exit) p->exit();
    return p;
}

const page_t *page_router_top(void)
{
    return (s_top > 0) ? s_stack[s_top - 1] : s_base;
}

bool page_router_overlay_active(void)
{
    return s_top > 0;
}

bool page_router_dispatch_button(nav_key_t id, button_event_t event)
{
    if (s_top == 0) return false;
    const page_t *p = s_stack[s_top - 1];
    if (p->on_button) p->on_button(id, event);   /* 返回值预留（见头注释） */
    return true;
}

void page_router_render_top(void)
{
    const page_t *p = page_router_top();
    if (p && p->render) p->render();
}
