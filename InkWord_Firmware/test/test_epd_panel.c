/**
 * @file test_epd_panel.c
 * @brief epd_panel 单测（开源通用化 Phase 1.5，2026-10-24）
 *
 * desc_check 违规用例矩阵（契约逐项：几何/帧预算/色彩平面/调色板/
 * 时序下限/ops 必填）+ 注册表 API（get_by_id/at/count 边界）。
 *
 * 链接说明：epd_panel.c 的 s_registry extern 引用 10 个 g_panel_*
 * （真身在 panels 目录的 .cpp，C++/ESP 硬件依赖 native 不可编）——本文件
 * 提供 10 个合法 stub desc 顶替链接位（同时让注册表 API 可被真测，
 * 且每个 stub 须经 desc_check 的免费断言）。
 *
 * 运行：pio test -e native-test（挂载见 test_srs_engine.c runner）
 */
#include <string.h>
#include <unity.h>
#include "epd_panel.h"

/* ---- 链接位：panels 目录 g_panel_* 的 native 顶替（均合法 desc） ---- */

static int stub_op_init(void) { return 0; }
static int stub_op_full(const uint8_t *f) { (void)f; return 0; }
static void stub_op_nop(void) { }

#define STUB_PANEL(sym, nm)                                     \
    const epd_panel_desc_t sym = {                              \
        .name = nm, .controller = EPD_CTRL_UC8253,              \
        .panel_w = 240, .panel_h = 416, .gfx_rotation = 1,      \
        .dpi = 150,                                             \
        .color_mode = EPD_COLOR_BW, .plane_count = 1,           \
        .palette = { [0] = 0, [1] = 1, [2] = 1, [3] = 1 },      \
        .accent_rgb = 0, .fb_location = EPD_FB_AUTO,            \
        .rst_pulse_ms = 5, .busy_level = 0,                     \
        .busy_timeout_ms = 3000,                                \
        .power_on_ms = 10, .power_off_ms = 10,                  \
        .full_ms = 2000, .partial_ms = 400,                     \
        .partial_enabled = false, .passes = 1,                \
        .partial_count_full_refresh = 8, .window_8align = false,\
        .ops = { .init = stub_op_init,                          \
                 .full_refresh = stub_op_full,                  \
                 .write_full = stub_op_full,                    \
                 .power_off = stub_op_nop,                      \
                 .deep_sleep = stub_op_nop },                   \
    }

STUB_PANEL(g_panel_depg0370, "depg0370_uc8253");
STUB_PANEL(g_panel_e042a13, "e042a13_ssd1619");
STUB_PANEL(g_panel_wf0270, "wf0270_ssd1680");
STUB_PANEL(g_panel_gdew027c44, "gdew027c44_il91874");
STUB_PANEL(g_panel_wft0290, "wft0290_bw");
STUB_PANEL(g_panel_opm021eb, "opm021eb_bw");
STUB_PANEL(g_panel_e042a13bw, "e042a13bw_ssd1619");
STUB_PANEL(g_panel_gdeq031t10, "gdeq031t10_uc8253");
STUB_PANEL(g_panel_hink_e0213a31, "hink_e0213a31_bw");
STUB_PANEL(g_panel_gdeq0426t82, "gdeq0426t82_ssd1677");

/* ---- desc_check：合法基准与违规矩阵 ---- */

static epd_panel_desc_t base_desc(void)
{
    epd_panel_desc_t d;
    memset(&d, 0, sizeof(d));
    d.name = "probe_ok";
    d.controller = EPD_CTRL_UC8253;
    d.panel_w = 240; d.panel_h = 416; d.gfx_rotation = 1; d.dpi = 150;
    d.color_mode = EPD_COLOR_BW; d.plane_count = 1;
    d.palette[1] = 1; d.palette[2] = 1; d.palette[3] = 1;
    d.fb_location = EPD_FB_AUTO;
    d.rst_pulse_ms = 5; d.busy_level = 0; d.busy_timeout_ms = 3000;
    d.power_on_ms = 10; d.power_off_ms = 10;
    d.full_ms = 2000; d.partial_ms = 400;
    d.partial_enabled = false; d.passes = 1;
    d.partial_count_full_refresh = 8; d.window_8align = false;
    d.ops.init = stub_op_init;
    d.ops.full_refresh = stub_op_full;
    d.ops.write_full = stub_op_full;
    d.ops.power_off = stub_op_nop;
    d.ops.deep_sleep = stub_op_nop;
    return d;
}

static int check(const epd_panel_desc_t *d)   /* err 缓冲 NULL 安全路径复用 */
{
    return epd_panel_desc_check(d, NULL, 0);
}

void test_panel_desc_check_accepts_valid(void)
{
    char err[64];
    epd_panel_desc_t d = base_desc();
    TEST_ASSERT_EQUAL_INT(0, epd_panel_desc_check(&d, err, sizeof(err)));
}

void test_panel_desc_check_name_violations(void)
{
    char err[64];
    epd_panel_desc_t d = base_desc();

    TEST_ASSERT_EQUAL_INT(-1, epd_panel_desc_check(NULL, err, sizeof(err)));

    d = base_desc(); d.name = NULL;
    TEST_ASSERT_EQUAL_INT(-1, check(&d));

    d = base_desc(); d.name = "";
    TEST_ASSERT_EQUAL_INT(-1, check(&d));
}

void test_panel_desc_check_geometry_ranges(void)
{
    epd_panel_desc_t d = base_desc();

    d = base_desc(); d.panel_w = 15;
    TEST_ASSERT_EQUAL_INT(-1, check(&d));
    d = base_desc(); d.panel_w = 2049;
    TEST_ASSERT_EQUAL_INT(-1, check(&d));
    d = base_desc(); d.panel_h = 15;
    TEST_ASSERT_EQUAL_INT(-1, check(&d));
    d = base_desc(); d.panel_h = 4097;
    TEST_ASSERT_EQUAL_INT(-1, check(&d));
    /* 合法边界值（帧预算内）：恰好 2048x4096 会被预算拦，取 2048x2046 */
    d = base_desc(); d.panel_w = 2048; d.panel_h = 2046;
    TEST_ASSERT_EQUAL_INT(0, check(&d));
}

void test_panel_desc_check_frame_budget(void)
{
    epd_panel_desc_t d = base_desc();
    d.panel_w = 2048; d.panel_h = 4096;      /* stride 256*4096 = 1MB > 512KB */
    TEST_ASSERT_EQUAL_INT(-1, check(&d));
}

void test_panel_desc_check_color_plane_pairs(void)
{
    epd_panel_desc_t d = base_desc();

    d = base_desc(); d.plane_count = 2;      /* BW 必须 1 平面 */
    TEST_ASSERT_EQUAL_INT(-1, check(&d));

    d = base_desc(); d.color_mode = EPD_COLOR_3C;   /* 3C 必须 2 平面 */
    TEST_ASSERT_EQUAL_INT(-1, check(&d));

    d = base_desc(); d.color_mode = EPD_COLOR_3C; d.plane_count = 2;  /* 合法 3C */
    TEST_ASSERT_EQUAL_INT(0, check(&d));

    d = base_desc(); d.color_mode = EPD_COLOR_6C; d.plane_count = 4;  /* 6C 越上界 */
    TEST_ASSERT_EQUAL_INT(-1, check(&d));
    d.color_mode = (epd_color_mode_t)99;     /* 非法枚举 */
    TEST_ASSERT_EQUAL_INT(-1, check(&d));
}

void test_panel_desc_check_palette_and_timing(void)
{
    epd_panel_desc_t d = base_desc();

    d = base_desc(); d.palette[0] = 2;       /* 1 平面时掩码须 <2 */
    TEST_ASSERT_EQUAL_INT(-1, check(&d));

    d = base_desc(); d.dpi = 0;              /* 除零风险项 */
    TEST_ASSERT_EQUAL_INT(-1, check(&d));

    d = base_desc(); d.rst_pulse_ms = 0;
    TEST_ASSERT_EQUAL_INT(-1, check(&d));

    d = base_desc(); d.busy_level = 2;
    TEST_ASSERT_EQUAL_INT(-1, check(&d));

    d = base_desc(); d.busy_timeout_ms = 99;
    TEST_ASSERT_EQUAL_INT(-1, check(&d));

    d = base_desc(); d.passes = 0;
    TEST_ASSERT_EQUAL_INT(-1, check(&d));

    d = base_desc(); d.partial_count_full_refresh = 0;
    TEST_ASSERT_EQUAL_INT(-1, check(&d));
}

void test_panel_desc_check_ops_mandatory(void)
{
    epd_panel_desc_t d = base_desc();

    d = base_desc(); d.ops.init = NULL;
    TEST_ASSERT_EQUAL_INT(-1, check(&d));
    d = base_desc(); d.ops.full_refresh = NULL;
    TEST_ASSERT_EQUAL_INT(-1, check(&d));
    d = base_desc(); d.ops.write_full = NULL;
    TEST_ASSERT_EQUAL_INT(-1, check(&d));
    d = base_desc(); d.ops.power_off = NULL;
    TEST_ASSERT_EQUAL_INT(-1, check(&d));
    d = base_desc(); d.ops.deep_sleep = NULL;
    TEST_ASSERT_EQUAL_INT(-1, check(&d));

    d = base_desc(); d.partial_enabled = true;   /* 局刷无实现 */
    TEST_ASSERT_EQUAL_INT(-1, check(&d));
}

void test_panel_registry_lookup_and_bounds(void)
{
    TEST_ASSERT_EQUAL_INT(10, epd_panel_registry_count());

    TEST_ASSERT_EQUAL_PTR(&g_panel_depg0370,
                          epd_panel_get_by_id("depg0370_uc8253"));
    TEST_ASSERT_EQUAL_PTR(&g_panel_gdeq0426t82,
                          epd_panel_get_by_id("gdeq0426t82_ssd1677"));
    TEST_ASSERT_NULL(epd_panel_get_by_id("nonexistent"));
    TEST_ASSERT_NULL(epd_panel_get_by_id(NULL));

    TEST_ASSERT_EQUAL_PTR(&g_panel_depg0370, epd_panel_at(0));
    TEST_ASSERT_EQUAL_PTR(&g_panel_gdeq0426t82, epd_panel_at(9));
    TEST_ASSERT_NULL(epd_panel_at(-1));
    TEST_ASSERT_NULL(epd_panel_at(10));
}

void test_panel_registry_stubs_contract_valid(void)
{
    /* 链接位 stub 自身合法（新屏 desc 契约的同款免费自检先例） */
    const epd_panel_desc_t *reg[] = {
        &g_panel_depg0370, &g_panel_e042a13, &g_panel_wf0270,
        &g_panel_gdew027c44, &g_panel_wft0290, &g_panel_opm021eb,
        &g_panel_e042a13bw, &g_panel_gdeq031t10, &g_panel_hink_e0213a31,
        &g_panel_gdeq0426t82,
    };
    for (int i = 0; i < 10; i++)
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, check(reg[i]), reg[i]->name);
}
