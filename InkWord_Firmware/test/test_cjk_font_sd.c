/**
 * @file test_cjk_font_sd.c
 * @brief SD 卡组子集字库级联模块测试（v1.4 T4.5，native-test）
 *
 * 手工构造 CKF1 fixture（n=2：饕 U+8C85 / 貔 U+992E，三级位图逐字形
 * 写特征字节），断言：装载三重校验（magic/几何/尺寸）拒坏件、三级
 * lookup 偏移正确（级间 stride*cell 步进 + 字形序步进）、二分 miss、
 * 越界级、文件不存在=1 的正常路径语义（主集已覆盖）、装载即卸旧。
 * 级联接线（cjk_text adv_one 主集 miss → 子集）由模块 API 语义 +
 * main.cpp load_active_words 编排保证，真机链路验证。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>
#include "cjk_font_sd.h"

/* setUp/tearDown 全 program 单份，由 test_word_parser_native.c 提供 */

/* storage_mount_point stub（cjk_font_sd.c 引用；host 无 SD 挂载语义） */
const char *storage_mount_point(void) { return "/tmp/inkword_cjk_sd_mount"; }

#define FIXTURE "/tmp/inkword_test_cjk_sd.bin"

/* fixture 布局常量（镜像 cjk_font_sd.c 三方同源几何）：
 * 24B 头 + 2*2B 码点 = 28B（已 4 对齐）
 * level0 base=28 (2*32B)、level1 base=92 (2*60B)、level2 base=212 (2*72B)
 * 总 356B。每字形首字节写特征值 0xA0+level*0x10+idx，尾字节写 0x5A */
static const uint32_t kLevelGlyphB[3] = { 32, 60, 72 };
static const uint32_t kCps[2] = { 0x8C85, 0x992E };

static void wr16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

/* 构造合法 fixture（cell0 篡改钩子：cell_override<0 保持 16） */
static int build_fixture(const char *path, int cell0_override, long trunc_to)
{
    uint32_t total = 28 + 2 * (32 + 60 + 72);
    uint8_t *b = calloc(1, total);
    memcpy(b, "CKF1", 4);
    wr16(b + 4, 1);
    wr16(b + 6, 3);
    wr32(b + 8, 2);
    wr16(b + 12, cell0_override >= 0 ? (uint16_t)cell0_override : 16);
    wr16(b + 14, 20); wr16(b + 16, 24);
    wr16(b + 18, 2);  wr16(b + 20, 3); wr16(b + 22, 3);
    wr16(b + 24, kCps[0]); wr16(b + 26, kCps[1]);
    uint32_t off = 28;
    for (int lvl = 0; lvl < 3; lvl++) {
        for (int i = 0; i < 2; i++) {
            b[off] = (uint8_t)(0xA0 + lvl * 0x10 + i);      /* 首字节特征 */
            b[off + kLevelGlyphB[lvl] - 1] = 0x5A;           /* 尾字节特征 */
            off += kLevelGlyphB[lvl];
        }
    }
    FILE *f = fopen(path, "wb");
    if (!f) { free(b); return -1; }
    long n = (trunc_to >= 0 && trunc_to < (long)total) ? trunc_to : (long)total;
    size_t w = fwrite(b, 1, (size_t)n, f);
    fclose(f);
    free(b);
    return w == (size_t)n ? 0 : -1;
}

void test_cjk_font_sd_load_and_lookup(void)
{
    TEST_ASSERT_EQUAL_INT(0, build_fixture(FIXTURE, -1, -1));
    TEST_ASSERT_EQUAL_INT(0, cjk_font_sd_load_file(FIXTURE));
    TEST_ASSERT_TRUE(cjk_font_sd_loaded());

    /* 三级命中 + 特征字节偏移正确（级内字形序步进 = stride*cell） */
    for (int lvl = 0; lvl < 3; lvl++) {
        for (int i = 0; i < 2; i++) {
            const uint8_t *bits = cjk_font_sd_lookup_level(kCps[i], lvl);
            char msg[64];
            snprintf(msg, sizeof(msg), "lvl=%d idx=%d", lvl, i);
            TEST_ASSERT_NOT_NULL_MESSAGE(bits, msg);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(
                (uint8_t)(0xA0 + lvl * 0x10 + i), bits[0], msg);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(
                0x5A, bits[kLevelGlyphB[lvl] - 1], msg);
        }
    }

    /* 二分 miss（区间外/区间内未收录）与越界级 */
    TEST_ASSERT_NULL(cjk_font_sd_lookup_level(0x8C84, 0));
    TEST_ASSERT_NULL(cjk_font_sd_lookup_level(0x992F, 2));
    TEST_ASSERT_NULL(cjk_font_sd_lookup_level(0x9000, 1));
    TEST_ASSERT_NULL(cjk_font_sd_lookup_level(kCps[0], 3));
    TEST_ASSERT_NULL(cjk_font_sd_lookup_level(kCps[0], -1));

    cjk_font_sd_unload();
    TEST_ASSERT_FALSE(cjk_font_sd_loaded());
    TEST_ASSERT_NULL(cjk_font_sd_lookup_level(kCps[0], 0));
}

void test_cjk_font_sd_rejects_bad_bin(void)
{
    /* 坏 magic：拒载且当前子集被卸载 */
    TEST_ASSERT_EQUAL_INT(0, build_fixture(FIXTURE, -1, -1));
    TEST_ASSERT_EQUAL_INT(0, cjk_font_sd_load_file(FIXTURE));
    TEST_ASSERT_TRUE(cjk_font_sd_loaded());

    FILE *f = fopen(FIXTURE, "r+b");
    TEST_ASSERT_NOT_NULL(f);
    fseek(f, 0, SEEK_SET);
    fwrite("XXXX", 1, 4, f);
    fclose(f);
    TEST_ASSERT_EQUAL_INT(-1, cjk_font_sd_load_file(FIXTURE));
    TEST_ASSERT_FALSE(cjk_font_sd_loaded());

    /* 几何篡改（cell0=18）拒载 */
    TEST_ASSERT_EQUAL_INT(0, build_fixture(FIXTURE, 18, -1));
    TEST_ASSERT_EQUAL_INT(-1, cjk_font_sd_load_file(FIXTURE));
    TEST_ASSERT_FALSE(cjk_font_sd_loaded());

    /* 截断（尺寸整账不符）拒载 */
    TEST_ASSERT_EQUAL_INT(0, build_fixture(FIXTURE, -1, 100));
    TEST_ASSERT_EQUAL_INT(-1, cjk_font_sd_load_file(FIXTURE));
    TEST_ASSERT_FALSE(cjk_font_sd_loaded());
}

void test_cjk_font_sd_missing_file_semantics(void)
{
    /* 文件不存在 = 1（主集已覆盖的正常路径），当前子集清空 */
    TEST_ASSERT_EQUAL_INT(0, build_fixture(FIXTURE, -1, -1));
    TEST_ASSERT_EQUAL_INT(0, cjk_font_sd_load_file(FIXTURE));
    TEST_ASSERT_TRUE(cjk_font_sd_loaded());

    TEST_ASSERT_EQUAL_INT(1, cjk_font_sd_load_file("/tmp/inkword_no_such.bin"));
    TEST_ASSERT_FALSE(cjk_font_sd_loaded());

    /* 空 deck_id（默认卡组）等价卸载 */
    TEST_ASSERT_EQUAL_INT(1, cjk_font_sd_load(""));
    remove(FIXTURE);
}

/* 跨实现直通验证：swift tools/gen_cjk_font.swift --subset 产物（Swift
 * 生成端）喂 C 消费端。两端独立实现仅以 CKF1 规格约束，本用例锁死
 * 同构契约：装载成功 + 从 bin 自身码点表取首/尾码点三级 lookup 命中。
 * 运行：DECK_BIN=<swift 产物> pio test -e native-test；缺省 IGNORE
 * （同 WORDS_JSON 纪律，无产物环境保绿） */
void test_cjk_font_sd_swift_artifact(void)
{
    const char *path = getenv("DECK_BIN");
    if (!path || !path[0]) {
        TEST_IGNORE_MESSAGE("set DECK_BIN=<swift subset artifact> to enable");
    }

    TEST_ASSERT_EQUAL_INT(0, cjk_font_sd_load_file(path));
    TEST_ASSERT_TRUE(cjk_font_sd_loaded());

    /* 从产物自身读首/尾码点（头 24B + u16[n]，小端），避免依赖生成内容 */
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(f);
    uint8_t head[28];
    TEST_ASSERT_EQUAL_size_t(28, fread(head, 1, 28, f));
    fclose(f);
    uint32_t n = (uint32_t)head[8] | ((uint32_t)head[9] << 8) |
                 ((uint32_t)head[10] << 16) | ((uint32_t)head[11] << 24);
    TEST_ASSERT_GREATER_THAN_UINT32(0, n);
    uint16_t cp0 = (uint16_t)(head[24] | (head[25] << 8));
    uint16_t cpz;                     /* 尾码点：fseek 独立取 */
    f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_INT(0, fseek(f, 24 + 2 * (long)n - 2, SEEK_SET));
    TEST_ASSERT_EQUAL_size_t(2, fread(&head[26], 1, 2, f));
    fclose(f);
    cpz = (uint16_t)(head[26] | (head[27] << 8));

    for (int lvl = 0; lvl < 3; lvl++) {
        TEST_ASSERT_NOT_NULL(cjk_font_sd_lookup_level(cp0, lvl));
        TEST_ASSERT_NOT_NULL(cjk_font_sd_lookup_level(cpz, lvl));
    }
    cjk_font_sd_unload();
}
