/**
 * @file test_catalog_index.c
 * @brief 教材目录索引纯 C 测试（2026-08-28，设计 §测试计划）
 *
 * 覆盖：年级分桶/排序序表、Unit 解析（末段 unit/无 unit/空 source）、
 * 桶内词库原序稳定、空词库/空 grade 归桶、超限并入兜底桶、
 * 幂等重建与 release 语义。运行：pio test -e native-test。
 */
#include <stdio.h>
#include <string.h>
#include <unity.h>
#include "catalog_index.h"
#include "word_parser.h"

/* 自构词条（不依赖 word_parser 运行期词池） */
static WordEntry mk(const char *text, const char *grade, const char *source)
{
    WordEntry w = { 0 };
    snprintf(w.text, sizeof(w.text), "%s", text);
    snprintf(w.grade, sizeof(w.grade), "%s", grade);
    snprintf(w.source, sizeof(w.source), "%s", source);
    return w;
}

void test_catalog_unit_name_of(void)
{
    char out[CATALOG_NAME_MAX];

    catalog_unit_name_of("人教版 九年级 Unit 5", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Unit 5", out);

    /* Starter 回看保留（不与正课单元合并），话题名保留到串尾 */
    catalog_unit_name_of("Starter Unit 2 What's this in English?", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Starter Unit 2 What's this in English?", out);

    catalog_unit_name_of("人教版 七年级上册 Starter Unit 1 Good morning!", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Starter Unit 1 Good morning!", out);

    /* 前置版本/年级丢弃，话题名保留（grade 层已承载学段） */
    catalog_unit_name_of("人教版 九年级 Unit 5 What are the shirts made of?", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Unit 5 What are the shirts made of?", out);

    catalog_unit_name_of("初中语文教材古诗文", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("初中语文教材古诗文", out);   /* 无 unit=整串 */

    catalog_unit_name_of("", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("(未分类)", out);

    catalog_unit_name_of(NULL, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("(未分类)", out);
}

void test_catalog_grade_buckets_and_order(void)
{
    WordEntry w[6];
    w[0] = mk("apple",   "九年级", "人教版 九年级 Unit 5");
    w[1] = mk("ability", "七年级上", "人教版 七年级上 Unit 1");
    w[2] = mk("zoo",     "九年级", "人教版 九年级 Unit 1");
    w[3] = mk("pen",     "",       "自选导入");
                 /* 空 grade → "(其他)" 桶 */
    w[4] = mk("egg",     "七年级上", "人教版 七年级上 Unit 1");
    w[5] = mk("cat",     "九年级", "仁爱版 九年级");

    int gn = catalog_build_from(6, w);
    TEST_ASSERT_EQUAL(3, gn);
    /* 序表：七年级上 < 九年级 < (其他) */
    TEST_ASSERT_EQUAL_STRING("七年级上", catalog_grade_name(0));
    TEST_ASSERT_EQUAL_STRING("九年级",   catalog_grade_name(1));
    TEST_ASSERT_EQUAL_STRING("(其他)",   catalog_grade_name(2));

    /* 桶内保持词库原序（教材导出顺序） */
    int n = 0;
    const uint16_t *e = catalog_unit_entries(1, 1, &n);   /* 九年级 → Unit 5 */
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL(0, e[0]);                            /* apple */

    e = catalog_unit_entries(1, 0, &n);                    /* 九年级 → Unit 1 */
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL(2, e[0]);                            /* zoo */

    /* 九年级单元序：Unit 1 < Unit 5 */
    TEST_ASSERT_EQUAL_STRING("Unit 1", catalog_unit(1, 0)->name);
    TEST_ASSERT_EQUAL_STRING("Unit 5", catalog_unit(1, 1)->name);
    /* 非 Unit 名（仁爱版）字典序在 Unit 后 */
    TEST_ASSERT_EQUAL(3, catalog_unit_count(1));
    TEST_ASSERT_EQUAL_STRING("仁爱版 九年级", catalog_unit(1, 2)->name);

    catalog_release();
}

void test_catalog_unit_order_numeric(void)
{
    WordEntry w[3];
    w[0] = mk("a", "八年级", "X Unit 10");
    w[1] = mk("b", "八年级", "X Unit 2");
    w[2] = mk("c", "八年级", "X Unit 1");

    TEST_ASSERT_EQUAL(1, catalog_build_from(3, w));
    /* 数值序：1 < 2 < 10（字典序会得 1,10,2） */
    TEST_ASSERT_EQUAL_STRING("Unit 1",  catalog_unit(0, 0)->name);
    TEST_ASSERT_EQUAL_STRING("Unit 2",  catalog_unit(0, 1)->name);
    TEST_ASSERT_EQUAL_STRING("Unit 10", catalog_unit(0, 2)->name);
    catalog_release();
}

void test_catalog_unit_topic_and_starter_order(void)
{
    /* 话题名后缀不阻数值序 + Starter 记负键排正课前（七上实形） */
    WordEntry w[5];
    w[0] = mk("name",    "七年级上", "Unit 1 My name's Gina.");
    w[1] = mk("morning", "七年级上", "Starter Unit 1 Good morning!");
    w[2] = mk("banana",  "七年级上", "Unit 6 Do you like bananas?");
    w[3] = mk("color",   "七年级上", "Starter Unit 3 What color is it?");
    w[4] = mk("sock",    "七年级上", "Unit 7 How much are these socks?");

    TEST_ASSERT_EQUAL(1, catalog_build_from(5, w));
    TEST_ASSERT_EQUAL(5, catalog_unit_count(0));
    /* Starter 1/3（负键）< Unit 1/6/7（数值） */
    TEST_ASSERT_EQUAL_STRING("Starter Unit 1 Good morning!", catalog_unit(0, 0)->name);
    TEST_ASSERT_EQUAL_STRING("Starter Unit 3 What color is it?", catalog_unit(0, 1)->name);
    TEST_ASSERT_EQUAL_STRING("Unit 1 My name's Gina.", catalog_unit(0, 2)->name);
    TEST_ASSERT_EQUAL_STRING("Unit 6 Do you like bananas?", catalog_unit(0, 3)->name);
    TEST_ASSERT_EQUAL_STRING("Unit 7 How much are these socks?", catalog_unit(0, 4)->name);
    catalog_release();
}

void test_catalog_grade_order_expansion_and_primary(void)
{
    /* 序表：小学最前；考纲拓展在九年级后、中考前；拓展桶独立可见 */
    WordEntry w[5];
    w[0] = mk("apple",   "考纲拓展", "考纲拓展词汇");
    w[1] = mk("poem",    "小学",     "小学必背古诗词");
    w[2] = mk("ability", "七年级上", "Unit 1 My name's Gina.");
    w[3] = mk("robot",   "九年级",   "Unit 7 Will people have robots?");
    w[4] = mk("exam",    "中考",     "中考考纲核心词汇");

    TEST_ASSERT_EQUAL(5, catalog_build_from(5, w));
    /* 序表：小学 < 七年级上 < 九年级 < 考纲拓展 < 中考 */
    TEST_ASSERT_EQUAL_STRING("小学",     catalog_grade_name(0));
    TEST_ASSERT_EQUAL_STRING("七年级上", catalog_grade_name(1));
    TEST_ASSERT_EQUAL_STRING("九年级",   catalog_grade_name(2));
    TEST_ASSERT_EQUAL_STRING("考纲拓展", catalog_grade_name(3));
    TEST_ASSERT_EQUAL_STRING("中考",     catalog_grade_name(4));
    catalog_release();
}

void test_catalog_empty_and_all_blank_grade(void)
{
    /* 空词库：0 桶、ready、查询安全 */
    TEST_ASSERT_EQUAL(0, catalog_build_from(0, NULL));
    TEST_ASSERT_TRUE(catalog_ready());
    TEST_ASSERT_EQUAL(0, catalog_grade_count());
    TEST_ASSERT_NULL(catalog_grade(0));

    /* grade 全空：单 "(其他)" 桶，词序不变 */
    WordEntry w[2];
    w[0] = mk("dog", "", "");
    w[1] = mk("pig", "", "");
    TEST_ASSERT_EQUAL(1, catalog_build_from(2, w));
    TEST_ASSERT_EQUAL_STRING("(其他)", catalog_grade_name(0));
    int n = 0;
    const uint16_t *e = catalog_unit_entries(0, 0, &n);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL(0, e[0]);
    TEST_ASSERT_EQUAL(1, e[1]);
    /* source 全空 → 单 "(未分类)" 单元 */
    TEST_ASSERT_EQUAL(1, catalog_unit_count(0));
    TEST_ASSERT_EQUAL_STRING("(未分类)", catalog_unit(0, 0)->name);
    catalog_release();
}

void test_catalog_overflow_merges_to_fallback(void)
{
    /* 17 个年级超限 → 第 17 个并入 "(其他)"（并保词序） */
    WordEntry w[17];
    char g[8];
    for (int i = 0; i < 17; i++) {
        snprintf(g, sizeof(g), "G%d", i);
        w[i] = mk("t", g, "");
    }
    int gn = catalog_build_from(17, w);
    TEST_ASSERT_EQUAL(CATALOG_GRADE_MAX, gn);
    /* "(其他)" 桶含 2 词（G16 并入 + 序表全未命中但容量满后 break 前
     * 的并入路径：G16 触发并入，"(其他)" 首建） */
    int total = 0;
    for (int j = 0; j < gn; j++) total += catalog_grade(j)->len;
    TEST_ASSERT_EQUAL(17, total);
    catalog_release();
}

void test_catalog_rebuild_idempotent(void)
{
    WordEntry w[2];
    w[0] = mk("apple", "九年级", "U Unit 1");
    w[1] = mk("bee",  "九年级", "U Unit 2");

    TEST_ASSERT_EQUAL(1, catalog_build_from(2, w));
    TEST_ASSERT_EQUAL(1, catalog_build_from(2, w));   /* 重复构建幂等 */
    int n = 0;
    const uint16_t *e = catalog_unit_entries(0, 0, &n);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL(0, e[0]);
    TEST_ASSERT_EQUAL(2, catalog_unit_count(0));
    catalog_release();
    TEST_ASSERT_FALSE(catalog_ready());
    TEST_ASSERT_EQUAL(0, catalog_grade_count());
}

void test_catalog_utf8_truncation_safe(void)
{
    /* 48B 缓冲截 57B 名（19 汉字）：完整 UTF-8 解码验证不切多字节中间
     * （注：完整汉字尾字节本身即 continuation，不能只查末字节） */
    const char *src = "初中语文教材古诗文超长单元名称测试串";
    WordEntry w[1];
    w[0] = mk("诗", "七年级下", src);
    TEST_ASSERT_EQUAL(1, catalog_build_from(1, w));
    const char *name = catalog_unit(0, 0)->name;
    size_t len = strlen(name);
    TEST_ASSERT_TRUE(len < strlen(src));   /* 确实发生截断 */
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)name[i];
        int step = (c < 0x80) ? 1 : (c & 0xE0) == 0xC0 ? 2 :
                   (c & 0xF0) == 0xE0 ? 3 : 4;
        char msg[64];
        snprintf(msg, sizeof(msg), "seq cut at %zu", i);
        TEST_ASSERT_MESSAGE(i + (size_t)step <= len, msg);
        i += (size_t)step;
    }
    catalog_release();
}
