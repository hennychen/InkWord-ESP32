/**
 * @file test_word_parser_native.c
 * @brief word_parser 云端导出词库回放测试（2026-08-23，双端验证纪律）
 *
 * 用后端 /api/admin/words/export 的真实 words.json（含默认词库 2407 条：
 * 开源中小学词库/古诗词，音标 ˈ、《》·等特殊字符密度高）喂固件解析器，
 * 断言：条数/难度/序号正确，11 字段与 cJSON 独立解析的原值逐字节相等
 * （即无 copy_str 截断、无 UTF-8 撕裂），cloudId 全量携带。
 *
 * 运行：WORDS_JSON=/path/words.json pio test -e native-test
 * 环境变量缺省时 IGNORE（保证无数据环境测试仍绿）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>
#include "word_parser.h"
#include "cJSON.h"

/* ---- host stub 实现（头在 test/stubs/，-I 顺序优先命中）---- */

void *heap_caps_malloc(size_t size, int caps)
{
    (void)caps;
    return malloc(size);
}

void heap_caps_free(void *ptr) { free(ptr); }

int storage_read_text(const char *path, char *out_buf, size_t buf_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(out_buf, 1, buf_size - 1, f);
    fclose(f);
    out_buf[n] = '\0';
    return (int)n;
}

/* ---- 测试主体 ---- */

/* 整文件读取（对照基准用；上限 4MB 防误吞大文件） */
static char *storage_read_text_alloc_for_test(const char *path);

/* 词池上限对齐固件 MAX_WORDS=4000（main.cpp）；1096B/条 ≈ 4.3MB host 可承受；
 * s_pool2 供 load_mem 与文件路径的同源对拍（两池 ≈8.6MB host 可承受） */
static WordEntry s_pool[4000];
static WordEntry s_pool2[4000];

/* 字段对照表：JSON key → WordEntry 字段缓冲 + 上限 */
typedef struct {
    const char *key;
    size_t offset;
    size_t cap;
} field_map_t;

/* JSON 键与成员名单独传：cloudId(camelCase) ≠ cloud_id(snake)，
 * 不能用 #member 隐式生成 JSON 键 */
#define FIELD(json_key, member, cap) \
    { json_key, offsetof(WordEntry, member), (cap) }

static const field_map_t kFields[] = {
    FIELD("text", text, WORD_TEXT_MAX),
    FIELD("phonetic", phonetic, WORD_PHONETIC_MAX),
    FIELD("meaning", meaning, WORD_MEANING_MAX),
    FIELD("example", example, WORD_EXAMPLE_MAX),
    FIELD("audio", audio, WORD_AUDIO_MAX),
    FIELD("tag", tag, WORD_TAG_MAX),
    FIELD("root", root, WORD_ROOT_MAX),
    FIELD("inflections", inflections, WORD_INFL_MAX),
    FIELD("source", source, WORD_SOURCE_MAX),
    FIELD("grade", grade, WORD_GRADE_MAX),
    FIELD("cloudId", cloud_id, WORD_CLOUD_ID_MAX),
};
#define FIELD_COUNT (sizeof(kFields) / sizeof(kFields[0]))

void setUp(void) {}
void tearDown(void) {}

void test_parse_cloud_export_words_json(void)
{
    const char *path = getenv("WORDS_JSON");
    if (!path || !path[0]) {
        TEST_IGNORE_MESSAGE("set WORDS_JSON=<backend export file> to enable");
    }

    int loaded = word_parser_load(path, s_pool, 4000);
    TEST_ASSERT_GREATER_THAN(0, loaded);
    TEST_ASSERT_EQUAL(word_parser_get_count(), loaded);

    /* 独立读源 JSON 作对照基准 */
    char *raw = storage_read_text_alloc_for_test(path);
    TEST_ASSERT_NOT_NULL(raw);
    cJSON *root = cJSON_Parse(raw);
    TEST_ASSERT_NOT_NULL(root);

    cJSON *jver = cJSON_GetObjectItem(root, "version");
    TEST_ASSERT_TRUE(cJSON_IsNumber(jver));
    int version = (int)jver->valuedouble;
    cJSON *words = cJSON_GetObjectItem(root, "words");
    TEST_ASSERT_TRUE(cJSON_IsArray(words));
    int total = cJSON_GetArraySize(words);

    /* 导出契约：loaded == total（词池容量内）、version == 条数 */
    TEST_ASSERT_EQUAL_INT(total, loaded);
    TEST_ASSERT_EQUAL_INT(version, loaded);

    int empty_cloud = 0, src_cloud = 0, utf8_weird = 0;
    for (int i = 0; i < loaded; i++) {
        cJSON *w = cJSON_GetArrayItem(words, i);
        TEST_ASSERT_TRUE(cJSON_IsObject(w));

        /* id 连续（设备本地序号，顺序稳定性依赖） */
        char msg[96];
        snprintf(msg, sizeof(msg), "id mismatch at %d", i);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)(i + 1), s_pool[i].id, msg);

        /* 11 字段逐字节全等：无截断 / 无 UTF-8 撕裂 / 无丢键 */
        for (size_t k = 0; k < FIELD_COUNT; k++) {
            const char *expect = cJSON_GetStringValue(
                cJSON_GetObjectItem(w, kFields[k].key));
            const char *actual =
                (const char *)((char *)&s_pool[i] + kFields[k].offset);
            snprintf(msg, sizeof(msg), "field '%s' diff at word %d",
                     kFields[k].key, i);
            TEST_ASSERT_EQUAL_STRING_MESSAGE(expect ? expect : "", actual, msg);
        }

        /* difficulty 对齐 */
        cJSON *jd = cJSON_GetObjectItem(w, "difficulty");
        snprintf(msg, sizeof(msg), "difficulty at %d", i);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(
            jd ? (uint8_t)jd->valueint : 1, s_pool[i].difficulty, msg);

        if (s_pool[i].cloud_id[0] == '\0') empty_cloud++;
        {
            const char *src_id = cJSON_GetStringValue(
                cJSON_GetObjectItem(w, "cloudId"));
            if (src_id && src_id[0]) src_cloud++;
        }

        /* 特殊字符密度抽查：音标重音符与中文书名号原样通过 */
        if (strstr(s_pool[i].phonetic, "\xcb\x88") /* ˈ */ ||
            strstr(s_pool[i].text, "\xe3\x80\x8a") /* 《 */) utf8_weird++;
    }

    cJSON_Delete(root);
    free(raw);

    /* cloudId 与源一致：导出产物全量携带（评分/收藏上报映射依赖）；
     * 内嵌兜底产物（gen_default_vocab.py 生成，无 cloudId 键）全空
     * = 本地词条不上报，两种产物均合法 —— 只断言非空数与源一致 */
    TEST_ASSERT_EQUAL_INT(src_cloud, loaded - empty_cloud);
    printf("[stat] loaded=%d, entries-with-IPA-or-title=%d\n",
           loaded, utf8_weird);
    TEST_ASSERT_GREATER_THAN(1000, utf8_weird);
}

static char *storage_read_text_alloc_for_test(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 4 * 1024 * 1024) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return NULL;
    }
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* 内存路径（load_mem，固件内嵌 rodata 入口）与文件路径同源对拍：
 * 同一份数据两路径加载，逐条整结构体内存全等（含 id/difficulty/
 * 全部字段；两路径均逐条 memset 后填充，无 padding 噪声） */
void test_word_parser_load_mem(void)
{
    const char *path = getenv("WORDS_JSON");
    if (!path || !path[0]) {
        TEST_IGNORE_MESSAGE("set WORDS_JSON=<backend export file> to enable");
    }

    char *raw = storage_read_text_alloc_for_test(path);
    TEST_ASSERT_NOT_NULL(raw);
    int n_mem = word_parser_load_mem(raw, strlen(raw), s_pool2, 4000);
    free(raw);
    TEST_ASSERT_GREATER_THAN(0, n_mem);
    TEST_ASSERT_EQUAL(word_parser_get_count(), n_mem);

    int n_file = word_parser_load(path, s_pool, 4000);
    TEST_ASSERT_EQUAL_INT(n_file, n_mem);

    for (int i = 0; i < n_mem; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "mem/file diff at word %d", i);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(
            &s_pool[i], &s_pool2[i], sizeof(WordEntry), msg);
    }
}

/* T4.1 协议 v2 兼容性固化（不用环境变量，永远执行）：v2 在旧 11 字段
 * 之外新增 subject/deckId/payloadType/front/back/payloadJson 六键，
 * 且可能携任意未来扩展（嵌套对象/数组/null 值）。word_parser 按名
 * 取值（cJSON_GetObjectItem），未知键天然忽略、值 null 时 GetStringValue
 * 返回 NULL → copy_str 空串兜底 —— 断言解析成功且既有字段不受污染。 */
void test_word_parser_ignores_protocol_v2_fields(void)
{
    static WordEntry v2pool[4];
    const char *json =
        "{\"version\":2,\"v\":2,\"words\":[{"
        "\"id\":1,\"cloudId\":\"00000000-0000-0000-0000-000000000001\","
        "\"text\":\"spring\",\"phonetic\":\"/sprɪŋ/\",\"meaning\":\"春天\","
        "\"example\":\"Spring is here.\",\"audio\":\"spring.mp3\","
        "\"tag\":\"七年级\",\"difficulty\":2,"
        "\"subject\":\"en\",\"deckId\":\"junior\","
        "\"payloadType\":\"word-card\",\"front\":\"spring\",\"back\":\"春天\","
        "\"payloadJson\":\"{\\\"lines\\\":[1,2]}\","
        "\"futureNested\":{\"a\":[true,null]},"
        "\"futureList\":[\"x\",\"y\"],"
        "\"futureNull\":null"
        "}]}";

    int n = word_parser_load_mem(json, strlen(json), v2pool, 4);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_UINT32(1, v2pool[0].id);
    TEST_ASSERT_EQUAL_STRING("spring", v2pool[0].text);
    TEST_ASSERT_EQUAL_STRING("春天", v2pool[0].meaning);
    TEST_ASSERT_EQUAL_STRING("/sprɪŋ/", v2pool[0].phonetic);
    TEST_ASSERT_EQUAL_STRING("00000000-0000-0000-0000-000000000001",
                             v2pool[0].cloud_id);
    TEST_ASSERT_EQUAL_UINT8(2, v2pool[0].difficulty);
}

/* T4.4 默写数据通路固化（设备端契约，永远执行）：poem-card 导出
 * 形态 = 平铺字段（text=下句答案 / root=上句题面 / phonetic=下句
 * 拼音 / meaning=译文，PoemSeeder 映射）+ payloadJson 结构化键
 * （{"prev":上句}，云端/App 消费，设备零解析）。断言四要素原样
 * 入 WordEntry，缺键空串兜底，payloadJson 字符串不污染既有字段。 */
void test_word_parser_poem_dictation_fields(void)
{
    static WordEntry pool[2];
    const char *json =
        "{\"version\":17,\"words\":[{"
        "\"id\":1,\"cloudId\":\"00000000-0000-0000-0000-0000000000e1\","
        "\"text\":\"疑是地上霜\","
        "\"phonetic\":\"yí shì dì shàng shuāng\","
        "\"meaning\":\"明月洒满窗前，恍如地上泛起一层秋霜\","
        "\"tag\":\"静夜思·李白\",\"difficulty\":1,"
        "\"root\":\"床前明月光\","
        "\"source\":\"部编版一年级上\",\"grade\":\"一年级\","
        "\"subject\":\"zh\",\"deckId\":\"poems\",\"payloadType\":\"poem-card\","
        "\"front\":\"疑是地上霜\",\"back\":\"明月洒满窗前，恍如地上泛起一层秋霜\","
        "\"payloadJson\":\"{\\\"prev\\\":\\\"床前明月光\\\"}\""
        "},{"
        "\"id\":2,\"text\":\"疑是银河落九天\","
        "\"phonetic\":\"yí shì yín hé luò jiǔ tiān\","
        "\"meaning\":\"瀑布飞泻仿佛是银河从九天之上倾落下来\","
        "\"root\":\"飞流直下三千尺\","
        "\"subject\":\"zh\",\"deckId\":\"poems\",\"payloadType\":\"poem-card\","
        "\"payloadJson\":\"{\\\"prev\\\":\\\"飞流直下三千尺\\\"}\""
        "}]}";

    int n = word_parser_load_mem(json, strlen(json), pool, 2);
    TEST_ASSERT_EQUAL_INT(2, n);

    TEST_ASSERT_EQUAL_STRING("疑是地上霜", pool[0].text);
    TEST_ASSERT_EQUAL_STRING("床前明月光", pool[0].root);
    TEST_ASSERT_EQUAL_STRING("yí shì dì shàng shuāng", pool[0].phonetic);
    TEST_ASSERT_EQUAL_STRING("明月洒满窗前，恍如地上泛起一层秋霜",
                             pool[0].meaning);
    TEST_ASSERT_EQUAL_STRING("静夜思·李白", pool[0].tag);
    TEST_ASSERT_EQUAL_STRING("部编版一年级上", pool[0].source);
    TEST_ASSERT_EQUAL_STRING("一年级", pool[0].grade);
    TEST_ASSERT_EQUAL_UINT8(1, pool[0].difficulty);

    TEST_ASSERT_EQUAL_STRING("疑是银河落九天", pool[1].text);
    TEST_ASSERT_EQUAL_STRING("飞流直下三千尺", pool[1].root);
    TEST_ASSERT_EQUAL_STRING("", pool[1].tag);   /* 缺键空串兜底 */

    /* payloadJson 字符串不解析不污染（v2 忽略语义同前用例） */
    TEST_ASSERT_EQUAL_UINT32(1, pool[0].id);
    TEST_ASSERT_EQUAL_UINT32(2, pool[1].id);
}

/* native 平台单 program 单 main：runner 统一在 test_srs_engine.c，
 * 本文件只提供测试函数（新用例需在 runner 补 extern + RUN_TEST）。 */
