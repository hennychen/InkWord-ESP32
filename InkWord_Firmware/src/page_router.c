/**
 * @file page_router.c
 * @brief 页面路由实现（T1.4）：覆盖层小栈 + base 委托
 *
 * 栈深上限 4：现役最深串联为 menu→settings 同级替换（深 1），
 * 预留 quiz/browse/voice/chat 等临时视图后续接入的极端叠层。
 * 调用方均在主 loop / 按键回调上下文，无并发（display_busy 渲染
 * 守卫与 claim/release 的 LAN 生命周期调用同上下文，原子 bool 读）。
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

bool page_router_exit(const page_t *p)
{
    if (!page_router_pop_if(p)) return false;
    page_router_render_top();
    return true;
}

const page_t *page_router_top(void)
{
    return (s_top > 0) ? s_stack[s_top - 1] : s_base;
}

/* 显示通道外部独占位（P2 注册制）：LAN 接收页/AP portal 自绘整帧
 * 期间 claim，退出 release（lan_display_server 四个生命周期函数
 * 同步置位，与 s_active 一一对应） */
static bool s_display_claimed = false;

void page_router_display_claim(void)
{
    s_display_claimed = true;
}

void page_router_display_release(void)
{
    s_display_claimed = false;
}

bool page_router_display_busy(void)
{
    return s_top > 0 || s_display_claimed;
}

bool page_router_top_owns_display(void)
{
    if (s_display_claimed) return true;
    return s_top > 0 && s_stack[s_top - 1]->owns_display;
}

bool page_router_dispatch_button(nav_key_t id, button_event_t event)
{
    if (s_top == 0) {
        /* base 页按键收编（P2 路由补完）：base 注册 on_button 时转发，
         * 返回其结果（true=消费）；未注册保持原 false 语义（调用方
         * 自理，如早期 init 前/探针 env） */
        if (s_base && s_base->on_button)
            return s_base->on_button(id, event);
        return false;
    }
    const page_t *p = s_stack[s_top - 1];
    if (p->on_button && !p->on_button(id, event)) {
        /* false=请求退出：编排层统一出栈+恢复渲染（P2 兑现头注释
         * 预留语义；带业务编排的退出页内自理后返回 true） */
        page_router_pop_if(p);
        page_router_render_top();
    }
    return true;
}

void page_router_render_top(void)
{
    const page_t *p = page_router_top();
    if (p && p->render) p->render();
}
