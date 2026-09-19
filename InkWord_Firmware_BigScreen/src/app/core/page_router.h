/**
 * @file page_router.h
 * @brief 页面路由（T1.4）：两级模型——base 页 + 覆盖层栈
 *
 * base 页（学习/阅读/待机，由 study_mode 分流决定）由 main.cpp 注册；
 * 覆盖层与临时视图实现 page_t 后经 page_router_push 一行接入，
 * 渲染/按键按栈顶独占分发，取代散落的 xxx_is_active() 手工 if 链
 * 与跨文件 extern ui_render_current 依赖（P1 收官后全库归一）。
 *
 * 协议约定：
 *   - enter 自绘首帧（push 时机 = 进入时机，push 内回调）；
 *   - exit 仅清态（pop_if 时机 = 退出时机，pop_if 内回调；渲染恢复
 *     由退出方显式调 render_top，保持「先清态后渲染」现状时序）；
 *   - render 为整页重绘入口（render_top 分发；模块自管局刷的覆盖层
 *     可置 NULL——栈顶期间 render_top 不可达的场景）；
 *   - on_button 返回 false = 请求退出：dispatch 统一执行 pop+render_top
 *     （P2 路由补完：原各页手写退出三连收敛于编排层；带业务编排的
 *     退出仍页内自理后返回 true，如 chat 的模式机归位）；
 *   - name 为页标识（日志/黄金帧页面 id 同源；无路由行为语义）。
 *
 * 栈串联制（2026-09-08 重构）：菜单保持入栈，子功能页入栈其上，
 * 退出=pop 一层 render_top 自动恢复上级（「从哪进退哪」）；覆盖层
 * 间不再「先 exit 后 enter」（互斥 overlay 时代纪律退役）。
 */
#ifndef INKWORD_PAGE_ROUTER_H
#define INKWORD_PAGE_ROUTER_H

#include "button_handler.h"   /* nav_key_t / button_event_t */

#ifdef __cplusplus
extern "C" {
#endif

/** 页面协议：覆盖层/临时视图的统一生命周期契约 */
typedef struct page {
    const char *name;         /**< 页标识（调试/黄金帧 id；无行为语义） */
    void (*render)(void);    /**< 整页渲染（含刷新策略；可 NULL=模块自管局刷） */
    bool (*on_button)(nav_key_t id, button_event_t event); /**< 栈顶按键独占；false=请求退出编排 */
    void (*enter)(void);     /**< 入栈回调：自绘首帧（可 NULL 跳过） */
    void (*exit)(void);      /**< 出栈回调：清态（可 NULL 跳过；渲染恢复由退出方自理） */
    bool owns_display;       /**< true=自绘整帧独占（menu/settings/wifi/   browse：栈顶期间渲染族必须让位）；false=复用渲染族
                               *   自绘内容区（quiz/chat/pron：栈顶期间渲染族
                               *   守卫应放行，否则自阻塞——缺陷修复实测教训） */
} page_t;

/** 注册 base 页（main setup 早期调用；render=学习/阅读/待机分流） */
void page_router_init(const page_t *base);

/** 覆盖层入栈（幂等：栈顶同页拒绝；调 enter 回调） */
void page_router_push(const page_t *p);

/** 栈顶为 p 才弹出（调 exit 回调）；返回弹出页，非栈顶/空栈返回 NULL */
const page_t *page_router_pop_if(const page_t *p);

/** 页面侧主动退出编排：pop_if 成功则 render_top（原各页手写两连
 *  收敛；非栈顶时零动作）；返回是否实际弹出 */
bool page_router_exit(const page_t *p);

/** 清空覆盖栈直达 base（栈串联重构 2026-09-08）：逐层 pop（各层
 *  exit 回调依次执行，自顶向下）；不调 render_top（渲染恢复时序
 *  约定同 pop_if——调用方自理，如 browse 选词 seek 自渲染词卡）。
 *  供「终结型动作」场景使用：菜单→目录→选词 seek 直达词卡，
 *  不逐层返回菜单 */
void page_router_pop_to_base(void);

/** 栈顶页；栈空返回 base（未 init 返回 NULL） */
const page_t *page_router_top(void);

/** 显示通道外部独占：LAN 接收页/AP portal 自绘整帧期间 claim，
 *  退出 release（与 lan_display_server 的 s_active 置/清同步，
 *  显示占用真相源收敛于路由单点） */
void page_router_display_claim(void);

/** 显示通道外部独占解除（与 claim 配对） */
void page_router_display_release(void);

/** 显示通道忙 = 覆盖层栈非空 || 外部独占：任何栈页都算占用（standby
 *  待机页轮换/刷新停发守卫用此产一义——待机页只与真覆盖层共存，
 *  quiz/chat 入栈期间也不能轮换）；栈状态直查需求由本函数覆盖 */
bool page_router_display_busy(void);

/** 显示被自绘页/LAN 独占 = 外部 claim || 栈顶页 owns_display：渲染族
 *  守卫（ui_render_word/pron/chat）专用——quiz/chat 栈顶时放行自绘，
 *  menu/settings/wifi/browse 栈顶时让位（与旧 overlay 语义对齐；
 *  与 display_busy 分工：any 页占用 vs 自绘独占，勿混用） */
bool page_router_top_owns_display(void);

/** 按键分发：栈非空转发栈顶 on_button（false=请求退出→统一 pop+render_top）；
 * 栈空且 base 注册了 on_button 则转发 base（返回其结果），否则 false */
bool page_router_dispatch_button(nav_key_t id, button_event_t event);

/** 渲染栈顶（空栈渲染 base）——ui_render_current 的对外接替语义 */
void page_router_render_top(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_PAGE_ROUTER_H */
