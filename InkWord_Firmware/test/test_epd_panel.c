/**
 * @file test_epd_panel.c
 * @brief epd_panel 单测（开源通用化 Phase 1.5，2026-10-24）
 *
 * desc_check 违规用例矩阵（契约逐项：几何/帧预算/色彩平面/调色板/
 * 时序下限/ops 必填）+ 注册表 API（get_by_id/at/count 边界）+
 * auto_detect 四阶级联（2026-09-16：bus_auto_detect_probe 桩结果注入，
 * 静默屏 GDEQ0426T82 判别路径覆盖）。
 *
 * 链接说明：epd_panel.c 的 s_registry extern 引用 11 个 g_panel_*
 * （真身在 panels 目录的 .cpp，C++/ESP 硬件依赖 native 不可编）——本文件
 * 提供 11 个合法 stub desc 顶替链接位（同时让注册表 API 可被真测，
 * 且每个 stub 须经 desc_check 的免费断言）。另提供
 * bus_auto_detect_probe 注入桩（真身在 panels/epd_bus.cpp，Arduino
 * 依赖）：s_probe_stub 控制探测结果，auto_detect 级联纯逻辑可测。
 *
 * 运行：pio test -e native-test（挂载见 test_srs_engine.c runner）
 */
#include <string.h>
#include <unity.h>
#include "epd_panel.h"
#include "panels/epd_bus.h"

/* ---- 链接位：panels 目录 g_panel_* 的 native 顶替（均合法 desc） ---- */

static int stub_op_init(void) { return 0; }
static int stub_op_full(const uint8_t *f) { (void)f; return 0; }
static void stub_op_nop(void) { }

/* stub 定义非 const：auto_detect 用例需临时定制 otp_signature /
 * rst_busy_quiet 字段（真 desc 为 const，链接符号不检查限定符，
 * epd_panel.c 侧 extern const 声明照常命中） */
#define STUB_PANEL(sym, nm)                                     \
    epd_panel_desc_t sym = {                                    \
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
STUB_PANEL(g_panel_e213a57, "e213a57_ssd1680");
STUB_PANEL(g_panel_gdeq0426t82, "gdeq0426t82_ssd1677");

/* ---- bus_auto_detect_probe 注入桩（auto_detect 级联 native 可测） ----
 * s_probe_stub_ret=-1：探测失败（默认）；置 0 时 s_probe_stub 作返回值 */
static bus_probe_result_t s_probe_stub;
static int s_probe_stub_ret = -1;

int bus_auto_detect_probe(bus_probe_result_t *out)
{
    if (s_probe_stub_ret != 0 || !out) return -1;
    *out = s_probe_stub;
    return 0;
}

static void probe_set(bool idle_high, bool pulse, uint8_t otp)
{
    memset(&s_probe_stub, 0, sizeof(s_probe_stub));
    s_probe_stub.cog_alive = true;
    s_probe_stub.busy_idle_high = idle_high;
    s_probe_stub.busy_saw_pulse = pulse;
    s_probe_stub.status_reg = otp;
    s_probe_stub_ret = 0;
}

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
    TEST_ASSERT_EQUAL_INT(11, epd_panel_registry_count());

    TEST_ASSERT_EQUAL_PTR(&g_panel_depg0370,
                          epd_panel_get_by_id("depg0370_uc8253"));
    TEST_ASSERT_EQUAL_PTR(&g_panel_gdeq0426t82,
                          epd_panel_get_by_id("gdeq0426t82_ssd1677"));
    TEST_ASSERT_NULL(epd_panel_get_by_id("nonexistent"));
    TEST_ASSERT_NULL(epd_panel_get_by_id(NULL));

    TEST_ASSERT_EQUAL_PTR(&g_panel_depg0370, epd_panel_at(0));
    TEST_ASSERT_EQUAL_PTR(&g_panel_gdeq0426t82, epd_panel_at(10));
    TEST_ASSERT_NULL(epd_panel_at(-1));
    TEST_ASSERT_NULL(epd_panel_at(11));
}

void test_panel_registry_stubs_contract_valid(void)
{
    /* 链接位 stub 自身合法（新屏 desc 契约的同款免费自检先例） */
    const epd_panel_desc_t *reg[] = {
        &g_panel_depg0370, &g_panel_e042a13, &g_panel_wf0270,
        &g_panel_gdew027c44, &g_panel_wft0290, &g_panel_opm021eb,
        &g_panel_e042a13bw, &g_panel_gdeq031t10, &g_panel_hink_e0213a31,
        &g_panel_e213a57, &g_panel_gdeq0426t82,
    };
    for (int i = 0; i < 11; i++)
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, check(reg[i]), reg[i]->name);
}

/* ---- auto_detect 四阶级联（2026-09-16）----
 * stub 字段语义：全 busy_level=0（UC 特征 idle HIGH），个别定制
 * otp_signature / rst_busy_quiet 模拟真 desc（stub 非 const 可写）。
 * 用例按 stub 实际字段构造 probe——测的是级联逻辑，非物理族语义；
 * 定制字段用后还原，防用例间泄漏 */

void test_panel_auto_detect_probe_fail_returns_null(void)
{
    s_probe_stub_ret = -1;
    TEST_ASSERT_NULL(epd_panel_auto_detect());
}

void test_panel_auto_detect_quiet_ssd16_hits_gdeq0426(void)
{
    /* GDEQ0426T82（SSD1677）：静默屏无指纹（otp 读回残留 0xFF）、
     * 无分辨率读回——前三阶全落空后静默阶唯一命中 */
    g_panel_gdeq0426t82.rst_busy_quiet = true;
    probe_set(true, false, 0xFF);
    TEST_ASSERT_EQUAL_PTR(&g_panel_gdeq0426t82, epd_panel_auto_detect());
    g_panel_gdeq0426t82.rst_busy_quiet = false;
}

void test_panel_auto_detect_otp_unique_first_stage(void)
{
    /* SSD1619 指纹 0x01（e042a13 stub 定制）：第一阶唯一命中。
     * busy_level 定制为 1（SSD 族 BUSY 空闲 LOW）——OTP 阶自
     * 2026-09-19 起只接受 SSD16xx 0x2F，UC 特征探针不进此阶 */
    g_panel_e042a13.otp_signature = 0x01;
    g_panel_e042a13.busy_level = 1;
    probe_set(false, true, 0x01);
    TEST_ASSERT_EQUAL_PTR(&g_panel_e042a13, epd_panel_auto_detect());
    g_panel_e042a13.otp_signature = 0;
    g_panel_e042a13.busy_level = 0;
}

void test_panel_auto_detect_uc_flg_rejects_otp_stage(void)
{
    /* UC 族探针（busy 空闲 HIGH）即便读回 0x13 也不进 OTP 阶：
     * 0x71 是 FLG 状态位（真机证伪：硬复位 0x13/软复位 0x12，
     * 且族内所有屏同值），拿它命中会把任意 UC 屏判给 opm021eb_bw。
     * stub 全 desc 同几何 240x416 → 第三阶碰撞，第四阶需静默，
     * 故期望 NULL 走 NVS fallback（2026-09-19 3.1" 劫持回归） */
    g_panel_opm021eb.otp_signature = 0x13;
    probe_set(true, true, 0x13);
    TEST_ASSERT_NULL(epd_panel_auto_detect());
    g_panel_opm021eb.otp_signature = 0;
}

void test_panel_auto_detect_unknown_returns_null(void)
{
    /* 未知指纹 + 有忙窗（静默阶不进）→ NULL 走 NVS fallback */
    probe_set(true, true, 0x77);
    TEST_ASSERT_NULL(epd_panel_auto_detect());
}
