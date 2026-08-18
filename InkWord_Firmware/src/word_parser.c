/**
 * @file word_parser.c
 * @brief JSON 词库解析实现 (Task F-14)，基于 cJSON。
 *
 * words.json 结构约定：
 * {
 *   "version": 3,
 *   "words": [
 *     {"id":1,"text":"hello","phonetic":"/həˈləʊ/","meaning":"你好",
 *      "example":"Hello!","audio":"hello.mp3","tag":"grade7","difficulty":1}
 *   ]
 * }
 */
#include "word_parser.h"
#include "storage_manager.h"
#include "debug_log.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "PARSER";

#define JSON_MAX_BYTES  (512 * 1024)   /* 词库 JSON 最大 512KB */

static WordEntry *s_entries = NULL;
static int        s_count = 0;

/* 安全拷贝：截断而非溢出 */
static void copy_str(char *dst, size_t dst_max, const char *src)
{
    if (!dst || dst_max == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, dst_max - 1);
    dst[dst_max - 1] = '\0';
}

int word_parser_load(const char *path, WordEntry *out_array, int max_count)
{
    if (!path || !out_array || max_count <= 0) return -1;

    /* 1. 读取文件到内存 */
    char *raw = malloc(JSON_MAX_BYTES);
    if (!raw) {
        LOG_E("alloc json buffer failed");
        return -1;
    }
    int n = storage_read_text(path, raw, JSON_MAX_BYTES);
    if (n <= 0) {
        LOG_E("read %s failed", path);
        free(raw);
        return -1;
    }

    /* 2. 解析 JSON */
    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root) {
        LOG_E("json parse error near: %s", cJSON_GetErrorPtr());
        return -1;
    }

    cJSON *words = cJSON_GetObjectItem(root, "words");
    if (!cJSON_IsArray(words)) {
        LOG_E("'words' is not an array");
        cJSON_Delete(root);
        return -1;
    }

    int total = cJSON_GetArraySize(words);
    int loaded = 0;
    for (int i = 0; i < total && loaded < max_count; i++) {
        cJSON *w = cJSON_GetArrayItem(words, i);
        if (!cJSON_IsObject(w)) continue;

        WordEntry *e = &out_array[loaded];
        memset(e, 0, sizeof(*e));

        e->id = (uint32_t)cJSON_GetObjectItem(w, "id")->valuedouble;
        copy_str(e->text,     WORD_TEXT_MAX,     cJSON_GetStringValue(cJSON_GetObjectItem(w, "text")));
        copy_str(e->phonetic, WORD_PHONETIC_MAX, cJSON_GetStringValue(cJSON_GetObjectItem(w, "phonetic")));
        copy_str(e->meaning,  WORD_MEANING_MAX,  cJSON_GetStringValue(cJSON_GetObjectItem(w, "meaning")));
        copy_str(e->example,  WORD_EXAMPLE_MAX,  cJSON_GetStringValue(cJSON_GetObjectItem(w, "example")));
        copy_str(e->audio,    WORD_AUDIO_MAX,    cJSON_GetStringValue(cJSON_GetObjectItem(w, "audio")));
        copy_str(e->tag,      WORD_TAG_MAX,      cJSON_GetStringValue(cJSON_GetObjectItem(w, "tag")));
        cJSON *diff = cJSON_GetObjectItem(w, "difficulty");
        e->difficulty = diff ? (uint8_t)diff->valueint : 1;

        loaded++;
    }

    cJSON_Delete(root);

    s_entries = out_array;
    s_count = loaded;
    LOG_I("loaded %d words from %s (total in json=%d)", loaded, path, total);
    return loaded;
}

int word_parser_get_count(void)
{
    return s_count;
}

int word_parser_load_demo(WordEntry *out_array, int max_count)
{
    /* 演示词：释义用 ASCII（学习页 FreeSans 字库无中文字形，
     * 音标含 IPA 字符同样缺字形，故留空不画） */
    static const struct {
        const char *text;
        const char *meaning;
    } k_demo[] = {
        { "serendipity", "n. the occurrence of events by chance in a happy or beneficial way" },
        { "ephemeral",   "adj. lasting for a very short time" },
        { "lucid",       "adj. easy to understand; clear and bright" },
        { "zenith",      "n. the highest point reached; peak" },
        { "quixotic",    "adj. extremely idealistic and unrealistic" },
    };
    int n = (int)(sizeof(k_demo) / sizeof(k_demo[0]));
    if (n > max_count) n = max_count;

    for (int i = 0; i < n; i++) {
        WordEntry *e = &out_array[i];
        memset(e, 0, sizeof(*e));
        e->id = (uint32_t)(i + 1);
        copy_str(e->text,    WORD_TEXT_MAX,    k_demo[i].text);
        copy_str(e->meaning, WORD_MEANING_MAX, k_demo[i].meaning);
        copy_str(e->tag,     WORD_TAG_MAX,     "demo");
        e->difficulty = 1;
    }

    s_entries = out_array;
    s_count = n;
    LOG_I("loaded %d demo words (no SD word DB)", n);
    return n;
}

const WordEntry *word_parser_get(int index)
{
    if (index < 0 || index >= s_count || !s_entries) return NULL;
    return &s_entries[index];
}
