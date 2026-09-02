/**
 * @file test_selftest_diff.c
 * @brief 黄金帧 diff 纯函数灵敏度测试（T2.2 修 D2）
 *
 * 计划验收口径："人为改动一个布局参数后对应页 FAIL"——布局参数变化
 * 至少翻转 1 个像素位，本测试锁死 1 字节（8 像素粒度下限）检出与
 * 首异偏移正确性；真机端到端（渲染→read→diff）灵敏度由 demo env
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
