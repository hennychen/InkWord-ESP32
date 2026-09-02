/**
 * @file test_epd_geom.c
 * @brief 显示链路几何纯函数真值表测试（T2.1，修 D1）
 *
 * 覆盖：转置四方向（单像素角点标记 + 伪随机图案 rot 抵消往返）、
 * 窗口映射（四方向 + 越界钳位）、调色板退化（BW 面板 ACCENT/AUX →
 * BLACK 位级，§9.5 承诺真值表）。非 8 整除宽（13x7）用例锁定位
 * 打包行为（122 宽 OPM021EB 同类几何）。
 */
#include <unity.h>
#include <stdio.h>
#include <string.h>
#include "epd_geom.h"

/* setUp/tearDown 由 test_word_parser_native.c 提供（native 平台
 * 单 program 单 main 纪律，勿重复定义避免 duplicate symbol） */

/* ---- 位打包 helper（与被测公式同约定：MSB-first） ---- */
static void set_px(uint8_t *buf, int gw, int x, int y)
{
    buf[y * ((gw + 7) / 8) + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));
}

static int get_px(const uint8_t *buf, int pstride, int x, int y)
{
    return (buf[y * pstride + (x >> 3)] >> (7 - (x & 7))) & 1;
}

/* 单点转置验证：gfx (cx0,cy0) → rot 期望 (px0,py0)，且平面内仅此一位 */
static void check_single_point(int gw, int gh, int rot,
                               int cx0, int cy0, int px0, int py0)
{
    uint8_t src[64] = { 0 };
    uint8_t plane[64] = { 0 };
    const int pstride = (((rot & 1) ? gh : gw) + 7) / 8;

    set_px(src, gw, cx0, cy0);
    epd_geom_transpose(src, gw, gh, plane, pstride, rot);

    char msg[96];
    snprintf(msg, sizeof(msg), "rot=%d expected set at (%d,%d)", rot, px0, py0);
    TEST_ASSERT_EQUAL_MESSAGE(1, get_px(plane, pstride, px0, py0), msg);

    /* 唯一性：遍历面板几何，置位总数 = 1 */
    const int pw = (rot & 1) ? gh : gw, ph = (rot & 1) ? gw : gh;
    int bits = 0;
    for (int y = 0; y < ph; y++)
        for (int x = 0; x < pw; x++)
            bits += get_px(plane, pstride, x, y);
    TEST_ASSERT_EQUAL_MESSAGE(1, bits, "transposed plane must hold exactly 1 bit");
}

void test_epd_geom_transpose_four_dirs(void)
{
    /* 13x7 非 8 整除宽；四角 + 内点各方向落点（§6.3 映射表）
     *   rot0: (cx,cy)→(cx,cy)
     *   rot1: (cx,cy)→(gh-1-cy, cx)
     *   rot2: (cx,cy)→(gw-1-cx, gh-1-cy)
     *   rot3: (cx,cy)→(cy, gw-1-cx) */
    const int GW = 13, GH = 7;
    check_single_point(GW, GH, 0, 0, 0, 0, 0);          /* 左上 */
    check_single_point(GW, GH, 0, 12, 6, 12, 6);        /* 右下 */
    check_single_point(GW, GH, 0, 5, 2, 5, 2);          /* 内点 */

    check_single_point(GW, GH, 1, 0, 0, GH - 1 - 0, 0); /* (0,0)→(6,0) */
    check_single_point(GW, GH, 1, 12, 6, 0, 12);        /* 右下→左上域 */
    check_single_point(GW, GH, 1, 5, 2, 4, 5);          /* 内点 */

    check_single_point(GW, GH, 2, 0, 0, GW - 1, GH - 1);
    check_single_point(GW, GH, 2, 12, 6, 0, 0);
    check_single_point(GW, GH, 2, 5, 2, 7, 4);

    check_single_point(GW, GH, 3, 0, 0, 0, GW - 1);
    check_single_point(GW, GH, 3, 12, 6, 6, 0);
    check_single_point(GW, GH, 3, 5, 2, 2, 7);
}

void test_epd_geom_transpose_roundtrip(void)
{
    /* 13x7 满幅确定性伪随机图案：rot1→rot3 抵消、rot2→rot2 抵消
     * （等价 GFXcanvas1 缓冲逐字节比对）；转置后几何互换
     * （13x7 --rot1--> 7x13 --rot3--> 13x7） */
    const int GW = 13, GH = 7;
    uint8_t src[64] = { 0 }, mid[64] = { 0 }, back[64] = { 0 };
    uint32_t seed = 0xC0FFEEu;
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++) {
            seed = seed * 1103515245u + 12345u;
            if ((seed >> 16) & 1) set_px(src, GW, x, y);
        }

    /* rot1：面板几何 7x13（pstride=1） */
    epd_geom_transpose(src, GW, GH, mid, (GH + 7) / 8, 1);
    /* rot3：把 mid 当 7x13 画布（stride=(7+7)/8=1）转回 13x7 */
    epd_geom_transpose(mid, GH, GW, back, (GW + 7) / 8, 3);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src, back, (size_t)(((GW + 7) / 8) * GH));

    /* rot2 自抵消（几何不变 13x7） */
    memset(mid, 0, sizeof(mid));
    memset(back, 0, sizeof(back));
    epd_geom_transpose(src, GW, GH, mid, (GW + 7) / 8, 2);
    epd_geom_transpose(mid, GW, GH, back, (GW + 7) / 8, 2);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src, back, (size_t)(((GW + 7) / 8) * GH));
}

void test_epd_geom_rect_four_dirs(void)
{
    /* 13x7 内窗口 (2,3,4,2) 四方向映射（§6.3 表逐项） */
    const int GW = 13, GH = 7;
    uint16_t x, y, w, h;

    epd_geom_rect_to_panel(2, 3, 4, 2, GW, GH, 0, &x, &y, &w, &h);
    TEST_ASSERT_EQUAL_UINT16(2, x); TEST_ASSERT_EQUAL_UINT16(3, y);
    TEST_ASSERT_EQUAL_UINT16(4, w); TEST_ASSERT_EQUAL_UINT16(2, h);

    epd_geom_rect_to_panel(2, 3, 4, 2, GW, GH, 1, &x, &y, &w, &h);
    TEST_ASSERT_EQUAL_UINT16(GH - 3 - 2, x); TEST_ASSERT_EQUAL_UINT16(2, y);
    TEST_ASSERT_EQUAL_UINT16(2, w); TEST_ASSERT_EQUAL_UINT16(4, h);

    epd_geom_rect_to_panel(2, 3, 4, 2, GW, GH, 2, &x, &y, &w, &h);
    TEST_ASSERT_EQUAL_UINT16(GW - 2 - 4, x); TEST_ASSERT_EQUAL_UINT16(GH - 3 - 2, y);
    TEST_ASSERT_EQUAL_UINT16(4, w); TEST_ASSERT_EQUAL_UINT16(2, h);

    epd_geom_rect_to_panel(2, 3, 4, 2, GW, GH, 3, &x, &y, &w, &h);
    TEST_ASSERT_EQUAL_UINT16(3, x); TEST_ASSERT_EQUAL_UINT16(GW - 2 - 4, y);
    TEST_ASSERT_EQUAL_UINT16(2, w); TEST_ASSERT_EQUAL_UINT16(4, h);
}

void test_epd_geom_rect_clamp(void)
{
    /* 越界钳位：负偏移收缩 + 超界裁边 + 完全出界零尺寸 */
    const int GW = 13, GH = 7;
    uint16_t x, y, w, h;

    epd_geom_rect_to_panel(-2, 1, 10, 3, GW, GH, 0, &x, &y, &w, &h);
    TEST_ASSERT_EQUAL_UINT16(0, x); TEST_ASSERT_EQUAL_UINT16(1, y);
    TEST_ASSERT_EQUAL_UINT16(8, w); TEST_ASSERT_EQUAL_UINT16(3, h);

    epd_geom_rect_to_panel(5, 2, 20, 10, GW, GH, 0, &x, &y, &w, &h);
    TEST_ASSERT_EQUAL_UINT16(5, x); TEST_ASSERT_EQUAL_UINT16(2, y);
    TEST_ASSERT_EQUAL_UINT16(8, w); TEST_ASSERT_EQUAL_UINT16(5, h);

    epd_geom_rect_to_panel(20, 20, 2, 2, GW, GH, 0, &x, &y, &w, &h);
    TEST_ASSERT_EQUAL_UINT16(0, w); TEST_ASSERT_EQUAL_UINT16(0, h);

    /* rot=1 下的钳位组合：(0,-3,4,10) → 钳位后 (0,0,4,7) →
     * px=gh-y-h=0 / py=0 / pw=7 / ph=4 */
    epd_geom_rect_to_panel(0, -3, 4, 10, GW, GH, 1, &x, &y, &w, &h);
    TEST_ASSERT_EQUAL_UINT16(0, x); TEST_ASSERT_EQUAL_UINT16(0, y);
    TEST_ASSERT_EQUAL_UINT16(7, w); TEST_ASSERT_EQUAL_UINT16(4, h);
}

void test_epd_geom_palette_degrade(void)
{
    /* §9.4/§9.5 真值表：B/W 层 WHITE 独占白位，BLACK/ACCENT/AUX
     * （及未定义色值）全部黑位——即 BW 面板 ACCENT/AUX 退化为
     * BLACK 的位级依据；AC 层 ACCENT 独占置位，其余全清 */
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, epd_geom_bw_layer_color(EPD_GFX_WHITE));
    TEST_ASSERT_EQUAL_UINT16(0x0000, epd_geom_bw_layer_color(EPD_GFX_BLACK));
    TEST_ASSERT_EQUAL_UINT16(0x0000, epd_geom_bw_layer_color(EPD_GFX_ACCENT));
    TEST_ASSERT_EQUAL_UINT16(0x0000, epd_geom_bw_layer_color(EPD_GFX_AUX));
    TEST_ASSERT_EQUAL_UINT16(0x0000, epd_geom_bw_layer_color(7));

    TEST_ASSERT_EQUAL_UINT16(0xFFFF, epd_geom_ac_layer_color(EPD_GFX_ACCENT));
    TEST_ASSERT_EQUAL_UINT16(0x0000, epd_geom_ac_layer_color(EPD_GFX_WHITE));
    TEST_ASSERT_EQUAL_UINT16(0x0000, epd_geom_ac_layer_color(EPD_GFX_BLACK));
    TEST_ASSERT_EQUAL_UINT16(0x0000, epd_geom_ac_layer_color(EPD_GFX_AUX));

    /* 退化等价（BW 面板语义）：ACCENT/AUX 与 BLACK 位级相同 */
    TEST_ASSERT_EQUAL_UINT16(epd_geom_bw_layer_color(EPD_GFX_BLACK),
                             epd_geom_bw_layer_color(EPD_GFX_ACCENT));
    TEST_ASSERT_EQUAL_UINT16(epd_geom_bw_layer_color(EPD_GFX_BLACK),
                             epd_geom_bw_layer_color(EPD_GFX_AUX));
}
