/**
 * @file test_lan_proto.c
 * @brief T2.3 LAN 直传协议 v2 帧分类/头解析 native 测试
 *
 * 覆盖：classify 四合法路径 + REJECT（错长度/BW 面板 v2-color 退化）；
 * parse 合法 BW/color 头 body_len 计算 + 五种拒绝（magic/版本/bpp/
 * 错尺寸/无 accent 平面）——对应计划验收「v2 错尺寸明确拒绝」。
 */
#include <stddef.h>
#include <stdint.h>
#include "unity.h"
#include "lan_proto.h"

/* 3.7" BW：416x240，单平面 52*240=12480；三色 fb_total=24960 */
#define FB_SIZE   12480u
#define FB_TOTAL  24960u
/* setUp/tearDown 全 program 唯一（test_word_parser_native.c），此处不定义 */

static void build_hdr(uint8_t hdr[8], uint8_t m0, uint8_t m1, uint8_t ver,
                      uint8_t bpp, int w, int h)
{
    hdr[0] = m0; hdr[1] = m1; hdr[2] = ver; hdr[3] = bpp;
    hdr[4] = (uint8_t)(w >> 8); hdr[5] = (uint8_t)(w & 0xFF);
    hdr[6] = (uint8_t)(h >> 8); hdr[7] = (uint8_t)(h & 0xFF);
}

/* 三色面板：v1/v1.5/v2-BW/v2-color 四路径 + 错长度拒绝 */
void test_lan_classify_color_panel_paths(void)
{
    TEST_ASSERT_EQUAL(LAN_FRAME_V1_BW,
                      lan_frame_classify(FB_SIZE, FB_SIZE, FB_TOTAL));
    TEST_ASSERT_EQUAL(LAN_FRAME_V15_COLOR,
                      lan_frame_classify(FB_TOTAL, FB_SIZE, FB_TOTAL));
    TEST_ASSERT_EQUAL(LAN_FRAME_V2_BW,
                      lan_frame_classify(FB_SIZE + 8, FB_SIZE, FB_TOTAL));
    TEST_ASSERT_EQUAL(LAN_FRAME_V2_COLOR,
                      lan_frame_classify(FB_TOTAL + 8, FB_SIZE, FB_TOTAL));
    TEST_ASSERT_EQUAL(LAN_FRAME_REJECT,
                      lan_frame_classify(FB_SIZE + 1, FB_SIZE, FB_TOTAL));
    TEST_ASSERT_EQUAL(LAN_FRAME_REJECT,
                      lan_frame_classify(0, FB_SIZE, FB_TOTAL));
}

/* BW 面板（fb_total==fb_size）：v2-color 长度不合法，退化 v1/v2-BW */
void test_lan_classify_bw_panel_degrades(void)
{
    TEST_ASSERT_EQUAL(LAN_FRAME_V1_BW,
                      lan_frame_classify(FB_SIZE, FB_SIZE, FB_SIZE));
    TEST_ASSERT_EQUAL(LAN_FRAME_V2_BW,
                      lan_frame_classify(FB_SIZE + 8, FB_SIZE, FB_SIZE));
    /* 8+2*fb_size 既非裸长也非 8+fb_size：bpp=2 超 BW 面板平面数 → 拒绝 */
    TEST_ASSERT_EQUAL(LAN_FRAME_REJECT,
                      lan_frame_classify(FB_SIZE * 2 + 8, FB_SIZE, FB_SIZE));
}

void test_lan_v2_header_valid_bw_and_color(void)
{
    uint8_t hdr[8];
    size_t body = 0;
    const char *err = "";
    build_hdr(hdr, 'I', 'W', 2, 1, 416, 240);
    TEST_ASSERT_EQUAL(0, lan_v2_header_parse(hdr, 416, 240, 1, &body, &err));
    TEST_ASSERT_EQUAL_size_t(12480, body);
    build_hdr(hdr, 'I', 'W', 2, 2, 416, 240);
    TEST_ASSERT_EQUAL(0, lan_v2_header_parse(hdr, 416, 240, 2, &body, &err));
    TEST_ASSERT_EQUAL_size_t(24960, body);
}

void test_lan_v2_header_rejections(void)
{
    uint8_t hdr[8];
    size_t body = 0;
    const char *err = "";
    build_hdr(hdr, 'X', 'W', 2, 1, 416, 240);                    /* bad magic */
    TEST_ASSERT_EQUAL(-1, lan_v2_header_parse(hdr, 416, 240, 1, &body, &err));
    build_hdr(hdr, 'I', 'W', 3, 1, 416, 240);                    /* ver 3 未支持 */
    TEST_ASSERT_EQUAL(-2, lan_v2_header_parse(hdr, 416, 240, 1, &body, &err));
    build_hdr(hdr, 'I', 'W', 2, 3, 416, 240);                    /* bpp 3 非法 */
    TEST_ASSERT_EQUAL(-3, lan_v2_header_parse(hdr, 416, 240, 1, &body, &err));
    build_hdr(hdr, 'I', 'W', 2, 1, 200, 200);                    /* 错尺寸 */
    TEST_ASSERT_EQUAL(-4, lan_v2_header_parse(hdr, 416, 240, 1, &body, &err));
    build_hdr(hdr, 'I', 'W', 2, 2, 416, 240);                    /* bpp2 超 BW 平面 */
    TEST_ASSERT_EQUAL(-5, lan_v2_header_parse(hdr, 416, 240, 1, &body, &err));
    /* W/H 越界编码（0x7FFF 超 uint8 平面字节的合理面板）：尺寸不匹配拒 */
    build_hdr(hdr, 'I', 'W', 2, 1, 0x7FFF, 0x7FFF);
    TEST_ASSERT_EQUAL(-4, lan_v2_header_parse(hdr, 416, 240, 2, &body, &err));
}
