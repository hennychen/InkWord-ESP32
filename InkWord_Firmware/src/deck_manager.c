/**
 * @file deck_manager.c
 * @brief 词书（Deck）卡组管理器实现（协议与语义见 deck_manager.h）
 *
 * cJSON 小 DOM（manifest ≤ 数 KB）走默认内部 RAM 分配器
 * （word_parser 大 JSON 强制 PSRAM 的场景在此不适用）；manifest
 * 坏字段（id 超长/缺 name/file）逐条跳过不致命，清单空退化为默认。
 */
#include "deck_manager.h"
#include "storage_manager.h"
#include "gpio_config.h"      /* SD_MOUNT_POINT */
#include "debug_log.h"

#include "cJSON.h"
#include "nvs.h"
#include "settings_keys.h"   /* P2b：NVS 键权威表 */

#include <stdio.h>
#include <string.h>

static const char *TAG = "DECK";

#define MANIFEST_PATH   SD_MOUNT_POINT "/decks/manifest.json"
#define MANIFEST_MAX_B  (8 * 1024)   /* 清单上限（16 卡组 × 元数据余量充足） */

/* ---- 模块状态（静态零初始化；[0] 默认卡组常驻） ---- */
static deck_info_t s_decks[DECK_MAX] = {
    /* id/file 空 = words.json/内嵌链路；subject/payload_type 泛化缺省 */
    { "", "默认词库", "", 0, "en", "word-card" },
};
static int  s_deck_n = 1;
static char s_active[DECK_ID_MAX + 1] = "";   /* NVS deck_active 镜像（scan 时读） */

/* NVS 活跃记录读写（"inkword" 命名空间 str；默认卡组不落键=键缺失即默认，
 * 与 set_* 键「缺失=默认不写值」策略一致） */
static void active_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    char buf[sizeof(s_active)];
    size_t len = sizeof(buf);
    if (nvs_get_str(h, NVS_KEY_DECK_ACTIVE, buf, &len) == ESP_OK)
        snprintf(s_active, sizeof(s_active), "%s", buf);
    nvs_close(h);
}

int deck_manager_scan(void)
{
    s_deck_n = 1;                       /* 默认卡组复位常驻 [0] */
    active_load();

    char *raw = malloc(MANIFEST_MAX_B);
    if (!raw) return s_deck_n;
    int n = storage_read_text(MANIFEST_PATH, raw, MANIFEST_MAX_B);
    if (n <= 0) {
        free(raw);                      /* 无清单：单默认卡组（开箱不变） */
        return s_deck_n;
    }

    cJSON *root = cJSON_ParseWithLength(raw, (size_t)n);
    free(raw);
    if (!root) {
        LOG_W("manifest parse failed, default deck only");
        return s_deck_n;
    }

    const cJSON *decks = cJSON_GetObjectItem(root, "decks");
    const cJSON *it;
    cJSON_ArrayForEach(it, decks) {
        if (s_deck_n >= DECK_MAX) {
            LOG_W("deck table full (%d), rest ignored", DECK_MAX);
            break;
        }
        const char *id   = cJSON_GetStringValue(cJSON_GetObjectItem(it, "id"));
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(it, "name"));
        const char *file = cJSON_GetStringValue(cJSON_GetObjectItem(it, "file"));
        if (!id || !id[0] || strlen(id) > DECK_ID_MAX || !name || !file) {
            LOG_W("deck entry invalid (id/name/file), skipped");
            continue;                   /* 坏条目跳过不致命 */
        }
        deck_info_t *d = &s_decks[s_deck_n++];
        memset(d, 0, sizeof(*d));
        snprintf(d->id, sizeof(d->id), "%s", id);
        snprintf(d->name, sizeof(d->name), "%s", name);
        /* file 相对 decks 根（协议惯例）拼绝对路径；绝对路径原样收 */
        if (file[0] == '/')
            snprintf(d->file, sizeof(d->file), "%s", file);
        else
            snprintf(d->file, sizeof(d->file),
                     SD_MOUNT_POINT "/decks/%s", file);

        /* 存在性校验：文件缺失的条目不进表（切过去必失败，不如不列） */
        if (!storage_file_exists(d->file)) {
            LOG_W("deck %s file missing: %s, skipped", d->id, d->file);
            s_deck_n--;
            memset(d, 0, sizeof(*d));
            continue;
        }
        const cJSON *cnt = cJSON_GetObjectItem(it, "count");
        d->count = cJSON_IsNumber(cnt) ? (int)cnt->valueint : 0;
        /* subject/payloadType：v1.4 T4.2 兑现（缺省/空 = en/word-card；
         * 超长截断，T4.3 渲染分派消费） */
        const char *sub = cJSON_GetStringValue(cJSON_GetObjectItem(it, "subject"));
        const char *pt  = cJSON_GetStringValue(cJSON_GetObjectItem(it, "payloadType"));
        snprintf(d->subject, sizeof(d->subject), "%s",
                 (sub && sub[0]) ? sub : "en");
        snprintf(d->payload_type, sizeof(d->payload_type), "%s",
                 (pt && pt[0]) ? pt : "word-card");
    }
    cJSON_Delete(root);
    LOG_I("decks scanned: %d (active=%s)", s_deck_n,
          s_active[0] ? s_active : "default");
    return s_deck_n;
}

int deck_manager_count(void)
{
    return s_deck_n;                    /* 静态零初始化=1（默认卡组） */
}

const deck_info_t *deck_manager_at(int idx)
{
    if (idx < 0 || idx >= s_deck_n) return NULL;
    return &s_decks[idx];
}

const char *deck_manager_active_id(void)
{
    return s_active;
}

int deck_manager_active_index(void)
{
    for (int i = 0; i < s_deck_n; i++)
        if (strcmp(s_decks[i].id, s_active) == 0) return i;
    return 0;                           /* 未记录/失配回默认 */
}

int deck_manager_find_index(const char *id)
{
    const char *target = (id && id[0]) ? id : "";
    for (int i = 0; i < s_deck_n; i++)
        if (strcmp(s_decks[i].id, target) == 0) return i;
    return -1;
}

const char *deck_manager_active_name(void)
{
    return s_decks[deck_manager_active_index()].name;
}

const char *deck_manager_active_subject(void)
{
    const deck_info_t *d = &s_decks[deck_manager_active_index()];
    return d->subject[0] ? d->subject : "en";   /* 空串防御回退 */
}

const char *deck_manager_active_payload_type(void)
{
    const deck_info_t *d = &s_decks[deck_manager_active_index()];
    return d->payload_type[0] ? d->payload_type : "word-card";
}

const char *deck_manager_active_file(void)
{
    return s_decks[deck_manager_active_index()].file[0]
               ? s_decks[deck_manager_active_index()].file
               : NULL;                  /* 默认卡组走既有装载链路 */
}

int deck_manager_switch(int idx)
{
    if (idx < 0 || idx >= s_deck_n) return -1;
    snprintf(s_active, sizeof(s_active), "%s", s_decks[idx].id);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    if (s_active[0])
        nvs_set_str(h, NVS_KEY_DECK_ACTIVE, s_active);
    else
        nvs_erase_key(h, NVS_KEY_DECK_ACTIVE);    /* 切回默认=删键（缺失即默认） */
    nvs_commit(h);
    nvs_close(h);
    return 0;
}

/* 条目字段写入：已有则替换，新对象则添加（Replace 对不存在的键是空操作） */
static void entry_set_str(cJSON *obj, const char *key, const char *val)
{
    if (cJSON_GetObjectItem(obj, key))
        cJSON_ReplaceItemInObject(obj, key, cJSON_CreateString(val));
    else
        cJSON_AddStringToObject(obj, key, val);
}

static void entry_set_num(cJSON *obj, const char *key, double val)
{
    if (cJSON_GetObjectItem(obj, key))
        cJSON_ReplaceItemInObject(obj, key, cJSON_CreateNumber(val));
    else
        cJSON_AddNumberToObject(obj, key, val);
}

int deck_manager_upsert(const char *id, const char *name, int count,
                        const char *subject, const char *payload_type)
{
    if (!id || !id[0] || strlen(id) > DECK_ID_MAX) return -1;
    if (!name || !name[0]) name = id;
    if (!subject || !subject[0]) subject = "en";
    if (!payload_type || !payload_type[0]) payload_type = "word-card";

    /* 读现有清单（无则空文档），upsert 后写回 */
    char *raw = malloc(MANIFEST_MAX_B);
    if (!raw) return -1;
    int n = storage_read_text(MANIFEST_PATH, raw, MANIFEST_MAX_B);

    cJSON *root = (n > 0) ? cJSON_ParseWithLength(raw, (size_t)n) : NULL;
    free(raw);
    if (!root) root = cJSON_CreateObject();
    cJSON *decks = cJSON_GetObjectItem(root, "decks");
    if (!decks) {
        decks = cJSON_AddArrayToObject(root, "decks");
    }

    /* 同 id 条目更新，否则追加（表满不致命：文件已落，仅拒绝登记） */
    cJSON *it = NULL, *hit = NULL;
    cJSON_ArrayForEach(it, decks) {
        const char *eid =
            cJSON_GetStringValue(cJSON_GetObjectItem(it, "id"));
        if (eid && strcmp(eid, id) == 0) {
            hit = it;
            break;
        }
    }
    if (!hit && cJSON_GetArraySize(decks) >= DECK_MAX - 1) {
        LOG_W("manifest full (%d), deck %s not registered", DECK_MAX - 1, id);
        cJSON_Delete(root);
        return -1;
    }
    if (!hit) {
        hit = cJSON_CreateObject();
        cJSON_AddItemToArray(decks, hit);
    }
    entry_set_str(hit, "id", id);
    entry_set_str(hit, "name", name);
    char file_rel[32];
    snprintf(file_rel, sizeof(file_rel), "%s/words.json", id);
    entry_set_str(hit, "file", file_rel);   /* 相对 decks 根惯例 */
    entry_set_num(hit, "count", count);
    entry_set_str(hit, "subject", subject);          /* v1.4 T4.2 */
    entry_set_str(hit, "payloadType", payload_type);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return -1;

    storage_mkdir_p(SD_MOUNT_POINT "/decks");
    int rc = storage_write_text(MANIFEST_PATH, out, strlen(out));
    free(out);
    if (rc != 0) return -1;

    deck_manager_scan();               /* 重扫刷新内存表（幂等） */
    LOG_I("deck upserted: %s (%s, %d words)", id, name, count);
    return 0;
}
