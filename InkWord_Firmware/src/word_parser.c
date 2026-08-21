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

#include "esp_heap_caps.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "PARSER";

/* 词库 JSON 最大 2MB（4000 词 × ~400B/条；词池 PSRAM 化后扩容，
 * PRD §7.2 容量红线 2026-08-20 解除）；缓冲显式进 PSRAM ——
 * 虽 SPIRAM_USE_MALLOC 下 >16KB malloc 自动落 PSRAM，显式声明
 * 防配置漂移，且与 reader_engine/词池内存策略一致 */
#define JSON_MAX_BYTES  (2 * 1024 * 1024)

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

    /* 1. 读取文件到内存（PSRAM，见 JSON_MAX_BYTES 注释） */
    char *raw = heap_caps_malloc(JSON_MAX_BYTES, MALLOC_CAP_SPIRAM);
    if (!raw) {
        LOG_E("alloc json buffer failed");
        return -1;
    }
    int n = storage_read_text(path, raw, JSON_MAX_BYTES);
    if (n <= 0) {
        LOG_E("read %s failed", path);
        heap_caps_free(raw);
        return -1;
    }

    /* 2. 解析 JSON（cJSON DOM 大块分配在 SPIRAM_USE_MALLOC 下自动落 PSRAM） */
    cJSON *root = cJSON_Parse(raw);
    heap_caps_free(raw);
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

        /* id 为可选数字键（旧格式词库可能缺失，缺失时退化为加载序号；
         * 原写法 cJSON_GetObjectItem(...)->valuedouble 对缺失键 NULL 解引用，
         * 已改为安全判空） */
        cJSON *jid = cJSON_GetObjectItem(w, "id");
        e->id = cJSON_IsNumber(jid) ? (uint32_t)jid->valuedouble
                                    : (uint32_t)(loaded + 1);
        copy_str(e->text,     WORD_TEXT_MAX,     cJSON_GetStringValue(cJSON_GetObjectItem(w, "text")));
        copy_str(e->phonetic, WORD_PHONETIC_MAX, cJSON_GetStringValue(cJSON_GetObjectItem(w, "phonetic")));
        copy_str(e->meaning,  WORD_MEANING_MAX,  cJSON_GetStringValue(cJSON_GetObjectItem(w, "meaning")));
        copy_str(e->example,  WORD_EXAMPLE_MAX,  cJSON_GetStringValue(cJSON_GetObjectItem(w, "example")));
        copy_str(e->audio,    WORD_AUDIO_MAX,    cJSON_GetStringValue(cJSON_GetObjectItem(w, "audio")));
        copy_str(e->tag,      WORD_TAG_MAX,      cJSON_GetStringValue(cJSON_GetObjectItem(w, "tag")));
        /* 词库扩展四字段（V2.1 §6.2，2026-08-20）：旧词库缺键时
         * GetStringValue 返 NULL，copy_str 落空串，天然向后兼容 */
        copy_str(e->root,        WORD_ROOT_MAX,   cJSON_GetStringValue(cJSON_GetObjectItem(w, "root")));
        copy_str(e->inflections, WORD_INFL_MAX,   cJSON_GetStringValue(cJSON_GetObjectItem(w, "inflections")));
        copy_str(e->source,      WORD_SOURCE_MAX, cJSON_GetStringValue(cJSON_GetObjectItem(w, "source")));
        copy_str(e->grade,       WORD_GRADE_MAX,  cJSON_GetStringValue(cJSON_GetObjectItem(w, "grade")));
        /* 云端导出词库携带 Guid：评分/收藏上报的 WordId 映射（P2） */
        copy_str(e->cloud_id, WORD_CLOUD_ID_MAX, cJSON_GetStringValue(cJSON_GetObjectItem(w, "cloudId")));
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
    /* 演示词：中文释义验证词卡 16px 点阵混排（cjk_text，2026-08-20；
     * 音标含 IPA 字符，点阵与 FreeSans 均无字形，故留空不画）。
     * 词库扩展四字段样例：root 词根行 / grade·source 底部标签行 */
    static const struct {
        const char *text;
        const char *meaning;
        const char *tag;
        const char *root;
        const char *grade;
    } k_demo[] = {
        { "serendipity", "n. 意外发现美好事物的运气；机缘巧合", "中考核心", "ser=联系; serendip=珍宝", "九年级" },
        { "ephemeral",   "adj. 短暂的；转瞬即逝的",              "中考核心", "epi=在…上; hemer=白天", "九年级" },
        { "lucid",       "adj. 清晰易懂的；清澈的",              "中考核心", "luc=光; id=形容词尾", "九年级" },
        { "zenith",      "n. 顶点；鼎盛时期",                    "中考核心", "", "九年级" },
        { "quixotic",    "adj. 不切实际的；异想天开的",          "中考核心", "", "九年级" },
    };
    int n = (int)(sizeof(k_demo) / sizeof(k_demo[0]));
    if (n > max_count) n = max_count;

    for (int i = 0; i < n; i++) {
        WordEntry *e = &out_array[i];
        memset(e, 0, sizeof(*e));
        e->id = (uint32_t)(i + 1);
        copy_str(e->text,    WORD_TEXT_MAX,    k_demo[i].text);
        copy_str(e->meaning, WORD_MEANING_MAX, k_demo[i].meaning);
        copy_str(e->tag,     WORD_TAG_MAX,     k_demo[i].tag);
        copy_str(e->root,    WORD_ROOT_MAX,    k_demo[i].root);
        copy_str(e->grade,   WORD_GRADE_MAX,   k_demo[i].grade);
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
