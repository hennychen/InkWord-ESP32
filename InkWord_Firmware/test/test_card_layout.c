/**
 * @file test_card_layout.c
 * @brief card_layout payloadType 映射测试（v1.4 T4.3）
 *
 * 固化三版式映射与保守回退（NULL/空/未知/大小写敏感均回
 * word-card——英语开箱行为不变红线）。
 */
#include <unity.h>
#include "card_layout.h"

/* setUp/tearDown 由 test_word_parser_native.c 提供（native 平台
 * 单 program 单 main 纪律，勿重复定义避免 duplicate symbol） */

void test_card_layout_known_types(void)
{
    TEST_ASSERT_EQUAL_INT(CARD_LAYOUT_WORD,
                          card_layout_from_payload("word-card"));
    TEST_ASSERT_EQUAL_INT(CARD_LAYOUT_QA,
                          card_layout_from_payload("qa-card"));
    TEST_ASSERT_EQUAL_INT(CARD_LAYOUT_POEM,
                          card_layout_from_payload("poem-card"));
}

void test_card_layout_fallbacks(void)
{
    TEST_ASSERT_EQUAL_INT(CARD_LAYOUT_WORD, card_layout_from_payload(NULL));
    TEST_ASSERT_EQUAL_INT(CARD_LAYOUT_WORD, card_layout_from_payload(""));
    TEST_ASSERT_EQUAL_INT(CARD_LAYOUT_WORD,
                          card_layout_from_payload("flash-card"));
    TEST_ASSERT_EQUAL_INT(CARD_LAYOUT_WORD,
                          card_layout_from_payload("QA-CARD"));
}
