/**
 * @file test_page_router.c
 * @brief page_router 单测（开源通用化 Phase 1.5，2026-10-24）
 *
 * 覆盖层小栈全语义：push 幂等/栈深 4 溢出/pop_if 条件弹/claim 与
 * busy/top_owns_display 双语义/dispatch_button 三分支（NULL on_button
 * 吞键不弹栈——离线桩 page 设计依据；false 统一 pop+render_top；
 * base 转发）。回调计数器验证生命周期时序。
 *
 * 运行：pio test -e native-test（挂载见 test_srs_engine.c runner）
 */
#include <string.h>
#include <unity.h>
#include "page_router.h"

/* ---- 回调计数桩（每页独立计数器，designated init 顺序无关） ---- */

typedef struct {
    int enter, exit, render, on_button;
    bool on_button_ret;
} page_calls_t;

static page_calls_t s_ca, s_cb, s_cc, s_cd, s_ce, s_base;

static void reset_calls(page_calls_t *c) { memset(c, 0, sizeof(*c)); }

static void enter_a(void)  { s_ca.enter++; }
static void exit_a(void)   { s_ca.exit++; }
static void render_a(void) { s_ca.render++; }
static bool onbtn_a(nav_key_t id, button_event_t ev)
{ (void)id; (void)ev; s_ca.on_button++; return s_ca.on_button_ret; }

static void enter_b(void)  { s_cb.enter++; }
static void exit_b(void)   { s_cb.exit++; }
static void render_b(void) { s_cb.render++; }
static bool onbtn_b(nav_key_t id, button_event_t ev)
{ (void)id; (void)ev; s_cb.on_button++; return s_cb.on_button_ret; }

static void exit_c(void)   { s_cc.exit++; }
static void render_c(void) { s_cc.render++; }
static void exit_d(void)   { s_cd.exit++; }
static void render_d(void) { s_cd.render++; }
static void render_e(void) { s_ce.render++; }

static bool onbtn_base(nav_key_t id, button_event_t ev)
{ (void)id; (void)ev; s_base.on_button++; return s_base.on_button_ret; }

static const page_t s_pg_a = {
    .name = "a", .render = render_a, .on_button = onbtn_a,
    .enter = enter_a, .exit = exit_a, .owns_display = false,
};
static const page_t s_pg_b = {
    .name = "b", .render = render_b, .on_button = onbtn_b,
    .enter = enter_b, .exit = exit_b, .owns_display = true,
};
static const page_t s_pg_c = { .name = "c", .render = render_c,
                              .exit = exit_c };  /* on_button NULL：吞键语义 */
static const page_t s_pg_d = { .name = "d", .render = render_d };   /* enter/exit NULL：跳过安全 */
static const page_t s_pg_e = { .name = "e", .render = render_e };
static const page_t s_pg_base = {
    .name = "base", .on_button = onbtn_base, .owns_display = false,
};

/* ---- 状态复位（路由 static 状态跨用例存活；单 main 纪律下不能
 * 定义 setUp/tearDown（与 test_word_parser_native 重定义冲突），
 * 每例开头显式归零） ---- */

static void reset_router_state(void)
{
    page_router_init(&s_pg_base);
    page_router_pop_to_base();          /* 清栈（无 enter/exit 副作用关注） */
    page_router_display_release();
    reset_calls(&s_ca); reset_calls(&s_cb); reset_calls(&s_cc);
    reset_calls(&s_cd); reset_calls(&s_ce); reset_calls(&s_base);
}

void test_router_push_idempotent_top_repeat(void)
{
    reset_router_state();
    page_router_push(&s_pg_a);
    TEST_ASSERT_EQUAL_INT(1, s_ca.enter);
    page_router_push(&s_pg_a);          /* 栈顶同页：拒绝 */
    TEST_ASSERT_EQUAL_INT(1, s_ca.enter);
    TEST_ASSERT_EQUAL_PTR(&s_pg_a, page_router_top());
    page_router_push(&s_pg_b);
    page_router_push(&s_pg_b);          /* 换栈顶后重复 b 仍拒 */
    TEST_ASSERT_EQUAL_INT(1, s_cb.enter);
    TEST_ASSERT_EQUAL_PTR(&s_pg_b, page_router_top());
}

void test_router_push_null_and_overflow(void)
{
    reset_router_state();
    page_router_push(NULL);             /* NULL 拒绝：无崩溃零动作 */
    TEST_ASSERT_EQUAL_PTR(&s_pg_base, page_router_top());

    page_router_push(&s_pg_a);
    page_router_push(&s_pg_b);
    page_router_push(&s_pg_c);
    page_router_push(&s_pg_d);
    TEST_ASSERT_EQUAL_PTR(&s_pg_d, page_router_top());
    page_router_push(&s_pg_e);          /* 第 5 层：溢出拒绝（PAGE_STACK_MAX=4） */
    TEST_ASSERT_EQUAL_PTR(&s_pg_d, page_router_top());
    TEST_ASSERT_EQUAL_INT(0, s_ce.render);
}

void test_router_pop_if_only_top_match(void)
{
    reset_router_state();
    page_router_push(&s_pg_a);
    page_router_push(&s_pg_b);

    TEST_ASSERT_NULL(page_router_pop_if(&s_pg_a));   /* 非栈顶：零动作 */
    TEST_ASSERT_EQUAL_INT(0, s_ca.exit);
    TEST_ASSERT_EQUAL_PTR(&s_pg_b, page_router_top());

    TEST_ASSERT_EQUAL_PTR(&s_pg_b, page_router_pop_if(&s_pg_b));
    TEST_ASSERT_EQUAL_INT(1, s_cb.exit);             /* 栈顶命中：exit 回调 */
    TEST_ASSERT_EQUAL_PTR(&s_pg_a, page_router_top());

    TEST_ASSERT_NULL(page_router_pop_if(&s_pg_b));   /* 已不在栈：NULL */
}

void test_router_pop_to_base_exits_each_layer(void)
{
    reset_router_state();
    page_router_push(&s_pg_a);
    page_router_push(&s_pg_b);
    page_router_push(&s_pg_c);
    page_router_pop_to_base();
    TEST_ASSERT_EQUAL_INT(1, s_cc.exit);   /* 自顶向下逐层 exit */
    TEST_ASSERT_EQUAL_INT(1, s_cb.exit);
    TEST_ASSERT_EQUAL_INT(1, s_ca.exit);
    TEST_ASSERT_EQUAL_PTR(&s_pg_base, page_router_top());
    TEST_ASSERT_FALSE(page_router_display_busy());
}

void test_router_top_falls_to_base_and_null(void)
{
    reset_router_state();
    TEST_ASSERT_EQUAL_PTR(&s_pg_base, page_router_top());
    page_router_init(NULL);             /* 未 init 语义：top NULL */
    TEST_ASSERT_NULL(page_router_top());
}

void test_router_display_claim_semantics(void)
{
    reset_router_state();
    TEST_ASSERT_FALSE(page_router_display_busy());      /* 空栈未 claim */
    TEST_ASSERT_FALSE(page_router_top_owns_display());

    page_router_display_claim();
    TEST_ASSERT_TRUE(page_router_display_busy());       /* claim 独占（栈空也真） */
    TEST_ASSERT_TRUE(page_router_top_owns_display());

    page_router_display_release();
    TEST_ASSERT_FALSE(page_router_display_busy());
    TEST_ASSERT_FALSE(page_router_top_owns_display());
}

void test_router_top_owns_display_flag_semantics(void)
{
    reset_router_state();
    page_router_push(&s_pg_a);          /* owns_display=false */
    TEST_ASSERT_TRUE(page_router_display_busy());       /* 栈非空即 busy */
    TEST_ASSERT_FALSE(page_router_top_owns_display());  /* 位 false：复用渲染族 */

    page_router_push(&s_pg_b);          /* owns_display=true */
    TEST_ASSERT_TRUE(page_router_top_owns_display());   /* 位 true：渲染族让位 */

    page_router_pop_if(&s_pg_b);
    TEST_ASSERT_FALSE(page_router_top_owns_display());
}

void test_router_dispatch_null_on_button_swallows(void)
{
    reset_router_state();
    page_router_push(&s_pg_c);          /* on_button=NULL */
    TEST_ASSERT_TRUE(page_router_dispatch_button(NAV_CENTER, BUTTON_EVENT_SHORT_PRESS));
    TEST_ASSERT_EQUAL_PTR(&s_pg_c, page_router_top());  /* 吞键不弹栈（死页风险语义：
                                                         * 桩 page 必须带真实 on_button） */
    TEST_ASSERT_EQUAL_INT(0, s_cc.exit);
}

void test_router_dispatch_false_pops_and_renders(void)
{
    reset_router_state();
    page_router_push(&s_pg_a);
    page_router_push(&s_pg_b);
    s_cb.on_button_ret = false;         /* b 请求退出 */

    TEST_ASSERT_TRUE(page_router_dispatch_button(NAV_RST, BUTTON_EVENT_LONG_PRESS));
    TEST_ASSERT_EQUAL_INT(1, s_cb.on_button);
    TEST_ASSERT_EQUAL_INT(1, s_cb.exit);                /* 统一 pop */
    TEST_ASSERT_EQUAL_INT(1, s_ca.render);              /* + render_top 回上级 */
    TEST_ASSERT_EQUAL_PTR(&s_pg_a, page_router_top());
}

void test_router_dispatch_true_consumed_keeps_stack(void)
{
    reset_router_state();
    page_router_push(&s_pg_a);
    s_ca.on_button_ret = true;          /* 页内自理 */
    TEST_ASSERT_TRUE(page_router_dispatch_button(NAV_UP, BUTTON_EVENT_SHORT_PRESS));
    TEST_ASSERT_EQUAL_INT(1, s_ca.on_button);
    TEST_ASSERT_EQUAL_PTR(&s_pg_a, page_router_top());  /* 不弹不重绘 */
    TEST_ASSERT_EQUAL_INT(0, s_ca.render);
}

void test_router_dispatch_base_forward_and_no_base(void)
{
    reset_router_state();
    s_base.on_button_ret = true;
    TEST_ASSERT_TRUE(page_router_dispatch_button(NAV_SET, BUTTON_EVENT_LONG_PRESS));
    TEST_ASSERT_EQUAL_INT(1, s_base.on_button);         /* 空栈转发 base */

    page_router_init(NULL);             /* 无 base 空栈：原 false 语义（探针 env） */
    TEST_ASSERT_FALSE(page_router_dispatch_button(NAV_SET, BUTTON_EVENT_SHORT_PRESS));
}
