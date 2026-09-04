/**
 * @file test_layout_profile.c
 * @brief 布局档位分派测试（P2c native-test 扩展，2026-09-03）
 *
 * layout_profile.c 经 build_src_filter 链入 native 单 program；外部
 * 依赖 epd_gfx_width/height 在本文件提供桩（stubs/epd_driver.h 声明）。
 * 档位首调缓存单次初始化，用例间经 layout_profile_test_reset() 清
 * 缓存重新分派。覆盖：短边阈值边界 / 旋转无关 / narrow_tiny 特判 /
 * 缓存指针稳定 / 四档字段健全性。
 */
#include <stdio.h>
#include <unity.h>

#include "layout_profile.h"
#include "epd_driver.h"      /* stubs/epd_driver.h 优先命中（桩实现） */

/* ---- epd_gfx 桩（stubs/epd_driver.h 声明的单实例实现） ---- */
int stub_gfx_w = 416, stub_gfx_h = 240;
int epd_gfx_width(void)  { return stub_gfx_w; }
int epd_gfx_height(void) { return stub_gfx_h; }

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
