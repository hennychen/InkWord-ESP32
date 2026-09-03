/**
 * @file test_selftest_diff.c
 * @brief 黄金帧 diff/mask 纯函数测试（T2.2 修 D2 / T3.2 mask）
 *
 * 计划验收口径："人为改动一个布局参数后对应页 FAIL"——布局参数变化
 * 至少翻转 1 个像素位，本测试锁死 1 字节（8 像素粒度下限）检出与
 * 首异偏移正确性；mask 用例锁死"区域差异不计数/非区域差异计灵敏"
 * 双语义；真机端到端（渲染→read→diff）灵敏度由 demo env
 * 自检序列承担（改 layout_profile 一字段重烧即 FAIL）。
 */
#include <unity.h>
#include <string.h>
#include "selftest_diff.h"

/* setUp/tearDown 由 test_word_parser_native.c 提供（native 平台
 * 单 program 单 main 纪律，勿重复定义避免 duplicate symbol） */

static uint8_t m_a[260], m_b[260];  /* 覆盖 >256：多字节长度路径 */

void test_selftest_diff_identical(void)
{
    memset(m_a, 0xA5, sizeof(m_a));
    memcpy(m_b, m_a, sizeof(m_a));
    long off = 12345;
    TEST_ASSERT_EQUAL_INT(0, selftest_diff_bytes(m_a, m_b, sizeof(m_a), &off));
    TEST_ASSERT_EQUAL_INT(-1, off);  /* 无差异时偏移写 -1 */
}

void test_selftest_diff_single_byte(void)
{
    memset(m_a, 0, sizeof(m_a));
    memcpy(m_b, m_a, sizeof(m_a));
    m_b[100] = 0xFF;                 /* 单字节 8 像素翻转 */
    long off = -1;
    TEST_ASSERT_EQUAL_INT(1, selftest_diff_bytes(m_a, m_b, sizeof(m_a), &off));
    TEST_ASSERT_EQUAL_INT(100, off);
}

void test_selftest_diff_multi_first_offset(void)
{
    memset(m_a, 0x0F, sizeof(m_a));
    memcpy(m_b, m_a, sizeof(m_a));
    m_b[3] ^= 0x10; m_b[7] ^= 0x01; m_b[259] ^= 0x80;
    long off = -1;
    TEST_ASSERT_EQUAL_INT(3, selftest_diff_bytes(m_a, m_b, sizeof(m_a), &off));
    TEST_ASSERT_EQUAL_INT(3, off);   /* 首异 = 最小偏移 */
}

void test_selftest_diff_null_off_safe(void)
{
    memset(m_a, 0, sizeof(m_a));
    memcpy(m_b, m_a, sizeof(m_a));
    m_b[0] = 1;
    TEST_ASSERT_EQUAL_INT(1, selftest_diff_bytes(m_a, m_b, sizeof(m_a), NULL));
    TEST_ASSERT_EQUAL_INT(0, selftest_diff_bytes(m_a, m_a, 0, NULL)); /* 空帧 */
}

/* ---- T3.2 mask 纯函数：64x32 帧（stride=8B，全帧 256B），rect
 * {8,4,16,8} 覆盖行 4..11 的字节 1..2；双侧同构施加后区域差异归零
 * （缓冲须按帧真实字节数 256 分配，谨防越界写） ---- */
static uint8_t m_f[256];
static const int s_rect[1][4] = { {8, 4, 16, 8} };

void test_selftest_mask_region_diff_ignored(void)
{
    memset(m_f, 0x00, sizeof(m_f));
    uint8_t b[sizeof(m_f)];
    memcpy(b, m_f, sizeof(b));
    b[6 * 8 + 1] = 0xFF;              /* mask 区域内翻转（x=8..15, y=6） */
    long off = -1;
    TEST_ASSERT_TRUE(selftest_diff_bytes(m_f, b, sizeof(m_f), &off) > 0);
    selftest_diff_mask_white(m_f, 64, 32, s_rect, 1);
    selftest_diff_mask_white(b, 64, 32, s_rect, 1);
    TEST_ASSERT_EQUAL_INT(0, selftest_diff_bytes(m_f, b, sizeof(m_f), &off));
}

void test_selftest_mask_outside_diff_kept(void)
{
    memset(m_f, 0x00, sizeof(m_f));
    uint8_t b[sizeof(m_f)];
    memcpy(b, m_f, sizeof(b));
    b[0] = 0x80;                      /* mask 区域外（x=0, y=0） */
    selftest_diff_mask_white(m_f, 64, 32, s_rect, 1);
    selftest_diff_mask_white(b, 64, 32, s_rect, 1);
    long off = -1;
    TEST_ASSERT_EQUAL_INT(1, selftest_diff_bytes(m_f, b, sizeof(m_f), &off));
    TEST_ASSERT_EQUAL_INT(0, off);
}

void test_selftest_mask_whitens_region(void)
{
    memset(m_f, 0xFF, sizeof(m_f));
    selftest_diff_mask_white(m_f, 64, 32, s_rect, 1);
    TEST_ASSERT_EQUAL_HEX8(0x00, m_f[4 * 8 + 1]);   /* 区域内清位 */
    TEST_ASSERT_EQUAL_HEX8(0x00, m_f[11 * 8 + 2]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, m_f[0]);           /* 区域外原样 */
    TEST_ASSERT_EQUAL_HEX8(0xFF, m_f[4 * 8 + 0]);
}

void test_selftest_mask_rect_clamped(void)
{
    const int bad[2][4] = { {-4, -2, 100, 100}, {60, 30, 40, 40} };
    memset(m_f, 0xFF, sizeof(m_f));
    selftest_diff_mask_white(m_f, 64, 32, bad, 2);  /* 不越界写 */
    TEST_ASSERT_EQUAL_HEX8(0x00, m_f[0]);           /* 首矩形钳位后覆盖 */
    TEST_ASSERT_EQUAL_HEX8(0x00, m_f[31 * 8 + 7]);
}
