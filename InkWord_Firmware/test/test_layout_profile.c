/**
 * @file test_layout_profile.c
 * @brief 布局档位分派测试（P2c native-test 扩展，2026-09-03）
 *
 * layout_profile.c 经 build_src_filter 链入 native 单 program；外部
 * 依赖 epd_gfx_width/height 在本文件提供桩（stubs/epd_driver.h 声明）。
 * 档位首调缓存单次初始化，用例间经 layout_profile_test_reset() 清
 * 缓存重新分派。覆盖：短边阈值边界 / 旋转无关 / narrow_tiny 特判 /
 * 缓存指针稳定 / 四档字段健全性 / form 形态轴（P2）/
 * PPI 自动层选级（2026-09-08）/ kb_scale 宽度钳制与 3.1" 320x240
 * 及 240x320 画布（2026-09-19）。
 */
#include <stdio.h>
#include <unity.h>

#include "layout_profile.h"
#include "epd_driver.h"      /* stubs/epd_driver.h 优先命中（桩实现） */

/* ---- epd_gfx 桩（stubs/epd_driver.h 声明的单实例实现） ---- */
int stub_gfx_w = 416, stub_gfx_h = 240;
int epd_gfx_width(void)  { return stub_gfx_w; }
int epd_gfx_height(void) { return stub_gfx_h; }

/* cjk_font 桩：src/cjk_font.c 未链 native（字形 bin 经 objcopy EMBED，
 * 宿主无嵌入符号）；自动层仅取 cell px——四级表查与 glyph_cell()
 * 读 bin 头同值（16/20/24/32，非线性：24→32 跳 8） */
int cjk_glyph_cell_size(int level)
{
    static const int cells[] = { 16, 20, 24, 32 };
    return (level >= 0 && level < 4) ? cells[level] : 0;
}

/* 设尺寸 → 清缓存 → 取档（用例内一次性分派） */
static const layout_profile_t *dispatch(int w, int h)
{
    layout_profile_test_reset();
    stub_gfx_w = w;
    stub_gfx_h = h;
    return layout_profile_get();
}

/* 阈值边界：短边 <140 TINY / <200 SMALL / <320 MID / >=320 LARGE
 * （真机面板：122x250、176x264、240x416/300x400、480x800 各档代表） */
void test_layout_dispatch_boundaries(void)
{
    TEST_ASSERT_EQUAL(LAYOUT_TINY,  dispatch(122, 250)->kind);
    TEST_ASSERT_EQUAL(LAYOUT_TINY,  dispatch(139, 250)->kind);
    TEST_ASSERT_EQUAL(LAYOUT_SMALL, dispatch(140, 180)->kind);
    TEST_ASSERT_EQUAL(LAYOUT_SMALL, dispatch(176, 264)->kind);
    TEST_ASSERT_EQUAL(LAYOUT_SMALL, dispatch(199, 264)->kind);
    TEST_ASSERT_EQUAL(LAYOUT_MID,   dispatch(200, 240)->kind);
    TEST_ASSERT_EQUAL(LAYOUT_MID,   dispatch(240, 416)->kind);
    TEST_ASSERT_EQUAL(LAYOUT_MID,   dispatch(319, 400)->kind);
    TEST_ASSERT_EQUAL(LAYOUT_MID,   dispatch(300, 400)->kind);  /* 4.2" 三色 */
    TEST_ASSERT_EQUAL(LAYOUT_LARGE, dispatch(320, 480)->kind);
    TEST_ASSERT_EQUAL(LAYOUT_LARGE, dispatch(480, 800)->kind);
}

/* 旋转无关：档位只看短边，横竖屏同档（epd_gfx_* 为旋转后 UI 尺寸） */
void test_layout_rotation_invariant(void)
{
    TEST_ASSERT_EQUAL(dispatch(240, 416)->kind, dispatch(416, 240)->kind);
}

/* TINY 内窄屏特判：<=122 宽辅助字级跟随正文，128 宽保持 16px 原口径
 * （2026-08-30 真机定稿）；narrow_tiny 非 TINY 档恒 0 */
void test_layout_narrow_tiny(void)
{
    TEST_ASSERT_EQUAL_INT(1, dispatch(122, 250)->narrow_tiny);
    TEST_ASSERT_EQUAL_INT(0, dispatch(128, 296)->narrow_tiny);
    TEST_ASSERT_EQUAL_INT(0, dispatch(176, 264)->narrow_tiny);
}

/* 形态轴（P2，2026-09-05）：同档横竖共存判据 —— MID 档 3.1" 竖
 * （240x320）与 4.2"/3.7" 横（400x300/416x240）同档不同形态；
 * 消费方读 form 勿再散落 h>w 手写比较（T1.5 收敛同理） */
void test_layout_form_axis(void)
{
    TEST_ASSERT_EQUAL(LAYOUT_FORM_PORTRAIT,  dispatch(122, 250)->form);  /* 2.13" 竖 TINY */
    TEST_ASSERT_EQUAL(LAYOUT_FORM_PORTRAIT,  dispatch(240, 320)->form);  /* 3.1" 竖 MID（首个同档共存） */
    TEST_ASSERT_EQUAL(LAYOUT_FORM_LANDSCAPE, dispatch(416, 240)->form);  /* 3.7" 横 MID */
    TEST_ASSERT_EQUAL(LAYOUT_FORM_LANDSCAPE, dispatch(400, 300)->form);  /* 4.2" 横 MID */
    TEST_ASSERT_EQUAL(LAYOUT_FORM_SQUARE,    dispatch(152, 152)->form);  /* 方屏预留（2.66" 类） */
}

/* 惰性缓存：同尺寸两次调用返回同一指针（调用方持引用跨次有效） */
void test_layout_cached_pointer_stable(void)
{
    layout_profile_test_reset();
    stub_gfx_w = 240;
    stub_gfx_h = 416;
    TEST_ASSERT_EQUAL_PTR(layout_profile_get(), layout_profile_get());
}

/* 健全性：四档遍历，几何/字号/局刷阈值字段非退化（hint_h 允 0 不测） */
void test_layout_profile_sanity_all_kinds(void)
{
    const int ws[] = { 122, 176, 240, 480 };
    for (unsigned i = 0; i < sizeof(ws) / sizeof(ws[0]); i++) {
        const layout_profile_t *p = dispatch(ws[i], 400);
        char label[32];
        snprintf(label, sizeof(label), "kind=%d", (int)p->kind);
        TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, p->status_h, label);
        TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, p->margin_x, label);
        TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, p->font_px_main, label);
        TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, p->partial_std, label);
        TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, p->partial_standby, label);
        TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, p->partial_wifi, label);
        TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, p->partial_menu, label);
    }
}

/* PPI 自动层（2026-09-08）：set_dpi 注入后主内容/释义按物理字高
 * （3.7/3.4mm）+ 行宽容量（每行≥8 全角字）选最近字库级。用例尾部
 * 恢复 dpi=0——test_reset 不清 dpi，防污染表值口径用例
 * （Unity 顺序执行，防御未来重排） */
void test_layout_ppi_auto_level(void)
{
    /* 4.26" 800x480 @219：主 32px(3.7mm)/释义 3.4mm→29px 就近级3；
     * 几何派生复现表值（item/hint/info_lh 三字段） */
    layout_profile_set_dpi(219);
    const layout_profile_t *p = dispatch(800, 480);
    TEST_ASSERT_EQUAL_INT(3, p->font_lvl_main);
    TEST_ASSERT_EQUAL_INT(32, p->font_px_main);
    TEST_ASSERT_EQUAL_INT(3, p->mean_level);
    TEST_ASSERT_EQUAL_INT(52, p->item_h);   /* cell32 + pad[LARGE]20 */
    TEST_ASSERT_EQUAL_INT(32, p->hint_h);   /* hint 级2: 24+8 */
    TEST_ASSERT_EQUAL_INT(40, p->info_lh);  /* cell32 + 8 */

    /* 7.5" 800x480 @150 外推：主 22px→级2(24px/4.1mm)、释义 20px→级1 */
    layout_profile_set_dpi(150);
    p = dispatch(800, 480);
    TEST_ASSERT_EQUAL_INT(2, p->font_lvl_main);
    TEST_ASSERT_EQUAL_INT(24, p->font_px_main);
    TEST_ASSERT_EQUAL_INT(1, p->mean_level);

    /* 现役复现（表值零变化）：MID 416x240@130 主/释义均级1 */
    layout_profile_set_dpi(130);
    p = dispatch(416, 240);
    TEST_ASSERT_EQUAL_INT(1, p->font_lvl_main);
    TEST_ASSERT_EQUAL_INT(1, p->mean_level);

    /* SMALL 264x176@117 复现级1 */
    layout_profile_set_dpi(117);
    TEST_ASSERT_EQUAL_INT(1, dispatch(264, 176)->font_lvl_main);

    /* 行宽容量约束主导：TINY 122 宽@135 物理字高 20px 钳 15px(122/8)→级0 */
    layout_profile_set_dpi(135);
    TEST_ASSERT_EQUAL_INT(0, dispatch(122, 250)->font_lvl_main);

    /* dpi=0 退表值（native 不注入路径）：LARGE 表值级3 */
    layout_profile_set_dpi(0);
    TEST_ASSERT_EQUAL_INT(3, dispatch(800, 480)->font_lvl_main);
}

/* kb_scale 横向钳制（2026-09-19，3.1" 320 宽适配）：档位表值只保证
 * 垂直约束，最宽键盘行（行 0 = 10×36+9×3 = 387px）放不下时按运行期
 * 宽度下调。现役屏必须零变化：416/400 放得下保持 100，SMALL 264 取
 * 垂直定档的 60（钳制值 66 不触发） */
void test_layout_kb_scale_width_fit(void)
{
    TEST_ASSERT_EQUAL_INT(100, dispatch(416, 240)->kb_scale);  /* 3.7" 基线 */
    TEST_ASSERT_EQUAL_INT(100, dispatch(400, 300)->kb_scale);  /* 4.2" */
    TEST_ASSERT_EQUAL_INT(60,  dispatch(264, 176)->kb_scale);  /* 2.7" 垂直档 */
    TEST_ASSERT_EQUAL_INT(80,  dispatch(320, 240)->kb_scale);  /* 3.1" 横 */
    TEST_ASSERT_EQUAL_INT(59,  dispatch(240, 320)->kb_scale);  /* 3.1" 竖 */

    /* 不变式：钳制后行 0 实宽（wifi_config_ui.c kb_pixel_rect 口径）
     * 必须 ≤ 可用宽（屏宽 - 8 居中余量），否则居中负 x、左键出屏 */
    const int ws[] = { 416, 400, 320, 264, 240, 122 };
    for (unsigned i = 0; i < sizeof(ws) / sizeof(ws[0]); i++) {
        int w = ws[i];
        int s = dispatch(w, 300)->kb_scale;
        int key_w = 36 * s / 100;
        int gap   = 3 * s / 100;
        int total = 10 * key_w + 9 * gap;
        char label[32];
        snprintf(label, sizeof(label), "w=%d scale=%d", w, s);
        TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, key_w, label);
        TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(w - 8, total, label);
    }
}

/* 3.1" 两块可达画布（desc gfx_rotation=1 → 320x240 横；用户改方向
 * → 240x320 竖）：同落 MID 档、形态各判；PPI 自动层 @129 复现 MID
 * 表值几何，说明该屏无需为字号/行高手工校准 */
void test_layout_mid_31_canvas(void)
{
    const layout_profile_t *land = dispatch(320, 240);
    TEST_ASSERT_EQUAL(LAYOUT_MID, land->kind);
    TEST_ASSERT_EQUAL(LAYOUT_FORM_LANDSCAPE, land->form);

    const layout_profile_t *port = dispatch(240, 320);
    TEST_ASSERT_EQUAL(LAYOUT_MID, port->kind);
    TEST_ASSERT_EQUAL(LAYOUT_FORM_PORTRAIT, port->form);

    layout_profile_set_dpi(129);
    const layout_profile_t *p = dispatch(320, 240);
    TEST_ASSERT_EQUAL_INT(1,  p->font_lvl_main);   /* 3.7mm@129→19px→级1 */
    TEST_ASSERT_EQUAL_INT(20, p->font_px_main);
    TEST_ASSERT_EQUAL_INT(1,  p->mean_level);
    TEST_ASSERT_EQUAL_INT(44, p->item_h);          /* cell20 + pad[MID]24 */
    TEST_ASSERT_EQUAL_INT(22, p->hint_h);
    TEST_ASSERT_EQUAL_INT(28, p->info_lh);
    layout_profile_set_dpi(0);
}
