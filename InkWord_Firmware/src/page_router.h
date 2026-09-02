/**
 * @file page_router.h
 * @brief 页面路由（T1.4）：两级模型——base 页 + 覆盖层栈
 *
 * base 页（学习/阅读/待机，由 study_mode 分流决定）由 main.cpp 注册；
 * 覆盖层与临时视图实现 page_t 后经 page_router_push 一行接入，
 * 渲染/按键按栈顶独占分发，取代散落的 xxx_is_active() 手工 if 链
 * 与跨文件 extern ui_render_current 依赖（menu_ui/settings_ui/
 * study_mode_machine 归零；quiz_ui/voice_search 沿用兼容别名过渡）。
 *
 * 协议约定：
 *   - enter 自绘首帧（push 时机 = 进入时机，push 内回调）；
 *   - exit 仅清态（pop_if 时机 = 退出时机，pop_if 内回调；渲染恢复
 *     由退出方显式调 render_top，保持「先清态后渲染」现状时序）；
 *   - render 为整页重绘入口（render_top 分发；模块自管局刷的覆盖层
 *     可置 NULL——栈顶期间 render_top 不可达的场景）；
 *   - on_button 返回值预留（false=请求编排层处理，chat/voice 退出
 *     编排先例），现阶段栈顶总消费、dispatch 忽略返回值。
 */
#ifndef INKWORD_PAGE_ROUTER_H
#define INKWORD_PAGE_ROUTER_H

#include "button_handler.h"   /* nav_key_t / button_event_t */

#ifdef __cplusplus
extern "C" {
#endif

/** 页面协议：覆盖层/临时视图的统一生命周期契约 */
typedef struct page {
    void (*render)(void);    /**< 整页渲染（含刷新策略；可 NULL=模块自管局刷） */
    bool (*on_button)(nav_key_t id, button_event_t event); /**< 栈顶按键独占 */
    void (*enter)(void);     /**< 入栈回调：自绘首帧（可 NULL 跳过） */
    void (*exit)(void);      /**< 出栈回调：清态（可 NULL 跳过；渲染恢复由退出方自理） */
} page_t;

/** 注册 base 页（main setup 早期调用；render=学习/阅读/待机分流） */
void page_router_init(const page_t *base);

/** 覆盖层入栈（幂等：栈顶同页拒绝；调 enter 回调） */
void page_router_push(const page_t *p);

/** 栈顶为 p 才弹出（调 exit 回调）；返回弹出页，非栈顶/空栈返回 NULL */
const page_t *page_router_pop_if(const page_t *p);

/** 栈顶页；栈空返回 base（未 init 返回 NULL） */
const page_t *page_router_top(void);

/** 覆盖层存在（栈非空）：待机页禁绘/入睡检查等查询方的统一口径 */
bool page_router_overlay_active(void);

/** 按键分发：栈非空转发栈顶 on_button 并返回 true；栈空返回 false
 *  （base 按键仍由 main on_button 处理，不经路由） */
bool page_router_dispatch_button(nav_key_t id, button_event_t event);

/** 渲染栈顶（空栈渲染 base）——ui_render_current 的对外接替语义 */
void page_router_render_top(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_PAGE_ROUTER_H */
