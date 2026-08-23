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

/* cJSON DOM 专用分配器：强制 PSRAM（见 word_parser_load_mem 注释）。
 * free 不需要专用版本 —— heap 指针带 region 头，free() 统一可释放 */
static void *wp_json_malloc(size_t sz)
{
    return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
}

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

    /* 1. 读取文件到内存（PSRAM，见 JSON_MAX_BYTES 注释），
     * 解析委托 word_parser_load_mem（文件/内存两路径同源） */
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

    int ret = word_parser_load_mem(raw, (size_t)n, out_array, max_count);
    heap_caps_free(raw);
    return ret;
}

int word_parser_load_mem(const char *json, size_t len,
                         WordEntry *out_array, int max_count)
{
    if (!json || len == 0 || !out_array || max_count <= 0) return -1;

    /* 2. 解析 JSON。cJSON DOM 落点修正（2026-08-23 真机 boot loop
     * 实证）：cJSON 节点为小分配（数十 B，含 strdup 的 key/value），
     * SPIRAM_USE_MALLOC 的 ALWAYSINTERNAL 阈值（16KB）下全部走内部
     * RAM —— 2400 词 DOM 约 2MB 直接打拜 320KB 内部堆，Wi-Fi AMPDU
     * esp_timer_create 恰为首个申请者替死（ESP_ERR_NO_MEM abort）。
     * hooks 把 DOM 重定向 PSRAM（解析期峰值 DOM ~2.2MB + 词池
     * 4.28MB < 8MB 充裕）；cJSON_Hooks 为进程全局，但此调用位于
     * setup 单线程窗口（BLE 配网已结束、LAN/OTA 未起），无并发
     * cJSON 用户（ota/sync/ble/lan 均在运行期各自任务）。所有
     * 出口恢复默认，运行期小 JSON 不受影响。ParseWithLength 吃
     * 精确长度：内存路径不要求 NUL 结尾，可直接吃固件内嵌 rodata
     * （_binary_src_default_words_json_*，无拷贝；ESP-IDF 捆绑
     * cJSON ≥1.7.15 提供） */
    cJSON_Hooks psram_hooks = { .malloc_fn = wp_json_malloc, .free_fn = free };
    cJSON_Hooks default_hooks = { .malloc_fn = malloc, .free_fn = free };
    cJSON_InitHooks(&psram_hooks);
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        cJSON_InitHooks(&default_hooks);
        LOG_E("json parse error near: %s", cJSON_GetErrorPtr());
        return -1;
    }

    cJSON *words = cJSON_GetObjectItem(root, "words");
    if (!cJSON_IsArray(words)) {
        LOG_E("'words' is not an array");
        cJSON_Delete(root);
        cJSON_InitHooks(&default_hooks);
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
    cJSON_InitHooks(&default_hooks);   /* 恢复默认：运行期小 JSON 回内部 RAM */

    s_entries = out_array;
    s_count = loaded;
    LOG_I("loaded %d words (total in json=%d)", loaded, total);
    return loaded;
}

int word_parser_get_count(void)
{
    return s_count;
}

int word_parser_load_demo(WordEntry *out_array, int max_count)
{
    /* 演示词：中文释义验证词卡 16px 点阵混排（cjk_text，2026-08-20）。
     * 音标：真 IPA（2026-08-24 记号补全后 ˈ ˌ ː 合成位图可渲，
     * 覆盖 ɪ θ 重音等高频字符，裸 IPA 显示层自动补 / /）；
     * · 中点记号由底部标签行 grade·source 间隔号验证。
     * 词库扩展四字段样例：root 词根行 / grade·source 底部标签行 */
    static const struct {
        const char *text;
        const char *phonetic;
        const char *meaning;
        const char *tag;
        const char *root;
        const char *grade;
    } k_demo[] = {
        { "serendipity", "ˌserənˈdɪpəti", "n. 意外发现珍宝的运气；机缘巧合；serendipitous 是形容词，指偶然发现美好事物的；源于 1754 年英国作家 Walpole 所作的波斯童话 The Three Princes of Serendip，三位王子总凭智慧与运气意外发现珍宝。", "中考核心", "ser=联系; serendip=珍宝", "九年级" },
        { "ephemeral",   "ɪˈfemərəl",     "adj. 短暂的；转瞬即逝的",              "中考核心", "epi=在…上; hemer=白天", "九年级" },
        { "lucid",       "ˈluːsɪd",       "adj. 清晰易懂的；清澈的",              "中考核心", "luc=光; id=形容词尾", "九年级" },
        { "zenith",      "ˈziːnɪθ",       "n. 顶点；鼎盛时期",                    "中考核心", "", "九年级" },
        { "quixotic",    "kwɪkˈsɑːtɪk",   "adj. 不切实际的；异想天开的",          "中考核心", "", "九年级" },
    };
    int n = (int)(sizeof(k_demo) / sizeof(k_demo[0]));
    if (n > max_count) n = max_count;

    for (int i = 0; i < n; i++) {
        WordEntry *e = &out_array[i];
        memset(e, 0, sizeof(*e));
        e->id = (uint32_t)(i + 1);
        copy_str(e->text,     WORD_TEXT_MAX,    k_demo[i].text);
        copy_str(e->phonetic, WORD_PHONETIC_MAX, k_demo[i].phonetic);
        copy_str(e->meaning,  WORD_MEANING_MAX, k_demo[i].meaning);
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
