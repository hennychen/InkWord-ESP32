/**
 * @file offline_stub_cloud.c
 * @brief CLOUD 轴桩（INKWORD_FEATURE_CLOUD=0，开源通用化 Phase 1）
 *
 * 顶替 sync_client.c / sync_session.cpp / ota_manager.c / audio_sync.c
 * 四个编译单元（offline env build_src_filter 排除真实现 +<stubs/>
 * 纳入本桩）。头文件原样保留——include 既有头保证签名严格一致。
 *
 * 桩语义（返回值走既有降级路径，业务文件零改动）：
 *   - sync_has_device_key=false + wifi_is_connected=false（wifi 桩）
 *     → study_mode_enter_chat / enter_voice_search 预检自然拒绝
 *     （menu_ui/shortcut 入口长震降级，现有口径）；
 *   - sync_fetch_http_time=0 → 待机页时间无效引文留白（内建逻辑）；
 *   - sync_fetch_weather 非 0 → 天气拉取失败（NVS 缓存兜底同样跳过）；
 *   - silent_heartbeat_session：**非空桩**——直接 power_enter_sleep
 *     回睡（复用 power_manager 收口：时钟 checkpoint/状态落盘/外设
 *     关断），等价"零会话心跳"：TIMER 唤醒→确认无网可会话→回睡，
 *     防止 offline 档定时唤醒变全量开机（屏幕刷新耗电）。
 *
 * 纪律：被顶替模块新增公开 API 时必须同步本桩（链接错误兜底提醒）。
 */
#include "sync_client.h"
#include "sync_session.h"
#include "ota_manager.h"
#include "audio_sync.h"
#include "power_manager.h"   /* silent_heartbeat_session 回睡（PM_HEARTBEAT_PERIOD_S） */

#include <stddef.h>

/* ---- sync_client.h ---- */

void sync_set_base_url(const char *url) { (void)url; }          /* STUB */
const char *sync_get_base_url(void)     { return "https://api.einkword.com"; }  /* STUB：设备信息页显示默认值 */
void sync_set_device_key(const char *key) { (void)key; }        /* STUB */
bool sync_has_device_key(void)          { return false; }       /* STUB：chat/voice 预检拒绝 */
const char *sync_get_device_key(void)   { return ""; }          /* STUB */

int sync_register(const char *mac, const char *name,
                  char *out_api_key, size_t key_len)             /* STUB：无网注册失败 */
{
    (void)mac; (void)name; (void)out_api_key; (void)key_len;
    return -1;
}

int sync_pull_words(int local_version, char *out_buf, int buf_size)  /* STUB */
{
    (void)local_version; (void)out_buf; (void)buf_size;
    return -1;
}

int sync_push_progress(const ProgressItem *items, int count)     /* STUB */
{
    (void)items; (void)count;
    return -1;
}

int sync_push_collect(const char *word_id, bool collected)       /* STUB */
{
    (void)word_id; (void)collected;
    return -1;
}

int sync_push_master(const char *word_id, bool mastered)         /* STUB */
{
    (void)word_id; (void)mastered;
    return -1;
}

int sync_heartbeat(int battery, const char *fw_ver)              /* STUB */
{
    (void)battery; (void)fw_ver;
    return -1;
}

int sync_fetch_weather(weather_info_t *out)                      /* STUB */
{
    (void)out;
    return -1;
}

int sync_fetch_chat_review(chat_review_t *out)                   /* STUB */
{
    (void)out;
    return -1;
}

int64_t sync_fetch_http_time(void)     { return 0; }             /* STUB：时间无效→引文留白 */

int sync_push_reading_progress(const char *book_key, uint32_t signature,
    int current_page, int total_pages, int font_level, int read_minutes)  /* STUB */
{
    (void)book_key; (void)signature; (void)current_page; (void)total_pages;
    (void)font_level; (void)read_minutes;
    return -1;
}

int sync_push_bookmarks(const char *book_key, uint32_t signature,
    const sync_bookmark_item_t *items, int count)                /* STUB */
{
    (void)book_key; (void)signature; (void)items; (void)count;
    return -1;
}

int sync_pull_book_list(char *out_buf, int buf_size)             /* STUB */
{
    (void)out_buf; (void)buf_size;
    return -1;
}

int sync_download_book(const char *book_key, const char *save_path)  /* STUB */
{
    (void)book_key; (void)save_path;
    return -1;
}

/* ---- sync_session.h ---- */

void sync_credentials_load(void)       { }                       /* STUB：无凭据可恢复 */
void sync_background_task_start(void)  { }                       /* STUB：无后台任务 */

void silent_heartbeat_session(void)    /* STUB：非空桩——直接回睡（见头注释） */
{
    power_enter_sleep(PM_HEARTBEAT_PERIOD_S);   /* 不返回：checkpoint/落盘/外设收口复用 */
}

/* ---- ota_manager.h ---- */

bool ota_check_for_update(char *out_url, int url_len, char *out_md5,
                          int md5_len, int *out_size)            /* STUB */
{
    (void)out_url; (void)url_len; (void)out_md5; (void)md5_len; (void)out_size;
    return false;
}

int ota_perform_upgrade(const char *url, const char *expect_md5) /* STUB */
{
    (void)url; (void)expect_md5;
    return -1;
}

int ota_mark_valid(void)               { return 0; }             /* STUB：无 OTA 分区流转 */

/* ---- audio_sync.h ---- */

int audio_sync_cloud_total(void)       { return 0; }             /* STUB：徽标"缺0/总0" */
int audio_sync_missing_cached(void)    { return 0; }             /* STUB */
int audio_sync_refresh_stats(void)     { return -1; }            /* STUB */
bool audio_sync_is_running(void)       { return false; }         /* STUB */
int audio_sync_progress(void)          { return 0; }             /* STUB */
int audio_sync_start(void)             { return -1; }            /* STUB：菜单入口失败降级 */
