/**
 * @file voice_search.c
 * @brief AI 语音查词状态机实现（设计见头注）
 *
 * HTTP 复用项目惯例（chat_upload 同构）：URL 前缀 http:/其余 选明文/
 * TLS、multipart 三段流式写（PSRAM 峰值纪律）、X-Device-Key 头。
 * 上传超时 20s（后端 ASR + 三级词库匹配，无 LLM/TTS 链路，较 chat 短）。
 *
 * 渲染刷新：任务状态迁移页全刷（页内容大变），result 候选移动局刷
 * （阈值经 refresh_scheduler 升级保养）；三色屏零渲染（头注降级）。
 */
#include "voice_search.h"
#include "study_mode_machine.h"
#include "word_parser.h"
#include "deck_manager.h"
#include "mic_recorder.h"
#include "sync_client.h"       /* base_url / device_key 配置源 */
#include "wifi_manager.h"
#include "haptic.h"
#include "debug_log.h"
#include "epd_driver.h"
#include "cjk_text.h"
#include "layout_profile.h"
#include "refresh_scheduler.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "VOICE";

#define VS_TASK_STACK        (6 * 1024)  /* chat 同栈（esp_http+cJSON 栈深同源） */
#define VS_TASK_PRIO         (4)         /* chat/pron 同级，低于 btn_scan(5) */
#define VS_MAX_MS            (3000)      /* 查词档录音上限（设计 §B2） */
#define VS_UPLOAD_TIMEOUT_MS (20000)     /* ASR + 词库三级匹配 */
#define VS_RESP_MAX          (1536)      /* 5 候选 × (text64+meaning60+cloud40) + 信封 */

/* 局刷保养阈值（menu_ui mu_partial_threshold 同款公式，2026-09-04
 * 由硬编码 10 公式化：desc 基准 × profile.partial_menu 400 / 100） */
static int vs_partial_threshold(void)
{
    const epd_panel_desc_t *pd = epd_panel_desc();
    int base = (pd && pd->partial_count_full_refresh > 0)
             ? pd->partial_count_full_refresh : 8;
    return base * layout_profile_get()->partial_menu / 100;
}

/* ---- 模块状态 ---- */
typedef struct {
    char text[WORD_TEXT_MAX];
    char meaning[64];                   /* 后端已截 60 字符 */
    char cloud_id[WORD_CLOUD_ID_MAX];
} voice_cand_t;

static volatile bool         s_active = false;
static volatile voice_state_t s_state = VOICE_IDLE;
static volatile bool s_round_req = false;   /* 触发一轮录音查询 */
static volatile bool s_cancel    = false;   /* 录音取消（RST） */
static volatile bool s_send_now  = false;   /* 提前收尾（中键） */
static volatile bool s_exit_req  = false;   /* 任务收尾退出 */

static int s_cand_n = 0;
static int s_cand_sel = 0;
static voice_cand_t s_cands[VOICE_CAND_MAX];
static char s_transcript[96];
static char s_hint[64];                     /* idle/result 页提示行 */

/* ---- 几何派生（T1.5 档位参数表：布局值查 profile，与 menu_ui
 * MU_* / browse_mode BR_* 同源同值；可用高/可见数派生式局部保留） ---- */
#define VS_TITLE_H  (layout_profile_get()->status_h)        /* 标题栏高（T1.5） */
#define VS_ITEM_H   (layout_profile_get()->item_h)          /* 候选行高（T1.5） */
#define VS_FONT_H   (layout_profile_get()->font_px_main)    /* 主内容字号 px（T1.5） */
#define VS_FONT_LVL (layout_profile_get()->font_lvl_main)   /* 主内容 cjk level（T1.5） */
#define VS_FONT_ASC (layout_profile_get()->ascii_size_main) /* ASCII FreeSans size（T1.5） */
#define VS_HINT_H   (layout_profile_get()->hint_h)          /* 底部提示栏高（TINY 省略；T1.5） */
#define VS_LIST_TOP (VS_TITLE_H + 2)
#define VS_LIST_H   (epd_gfx_height() - VS_TITLE_H - VS_HINT_H)
#define VS_VISIBLE  (VS_LIST_H / VS_ITEM_H)
#define VS_MARGIN_X (layout_profile_get()->margin_x)        /* 左右留白（T1.5） */
#define VS_ITEM_W   (epd_gfx_width() - 2 * VS_MARGIN_X)
#define VS_SB_W     4

/* ---- 渲染 ---- */

static void vs_flush(void)
{
    epd_power_on();
    epd_gfx_flush();
    epd_power_off();
    /* 状态迁移页真全刷等价清残影，计数归零（menu_ui 同款） */
    refresh_notify_full_done();
}

static void vs_draw_hint(void)
{
    if (VS_HINT_H == 0) return;
    epd_gfx_draw_hline(VS_MARGIN_X, epd_gfx_height() - VS_HINT_H,
                       epd_gfx_width() - 2 * VS_MARGIN_X, EPD_GFX_BLACK);
    cjk_text_draw(VS_MARGIN_X,
                  epd_gfx_height() - VS_HINT_H + (VS_HINT_H - 16) / 2,
                  0, s_hint[0] ? s_hint : "中 说话  RST 退出", EPD_GFX_BLACK);
}

static void vs_draw_title(void)
{
    epd_gfx_fill_rect(0, 0, epd_gfx_width(), VS_TITLE_H, EPD_GFX_WHITE);
    cjk_text_draw(VS_MARGIN_X, (VS_TITLE_H - VS_FONT_H) / 2,
                  VS_FONT_LVL, "语音查词", EPD_GFX_BLACK);
    epd_gfx_draw_hline(VS_MARGIN_X, VS_TITLE_H,
                       epd_gfx_width() - 2 * VS_MARGIN_X, EPD_GFX_BLACK);
}

/* 单态大字页（idle/recording/searching：居中主文案 + 副行 transcript/hint） */
static void vs_draw_center(const char *main, const char *sub)
{
    int cy = VS_LIST_TOP + VS_LIST_H / 2 - VS_FONT_H;
    cjk_text_draw((epd_gfx_width() -
                   cjk_text_width(VS_FONT_LVL, main)) / 2, cy,
                  VS_FONT_LVL, main, EPD_GFX_BLACK);
    if (sub && sub[0])
        cjk_text_draw((epd_gfx_width() -
                       cjk_text_width(0, sub)) / 2,
                      cy + VS_FONT_H + 10, 0, sub, EPD_GFX_BLACK);
}

/* result 候选列表（transcript 小字行 + 反选列表，超宽截断同 browse） */
static void vs_draw_result(void)
{
    int y = VS_LIST_TOP + 2;
    if (s_transcript[0]) {
        char tr[104];   /* 「」(6B) + s_transcript 95B + NUL */
        snprintf(tr, sizeof(tr), "「%s」", s_transcript);
        cjk_text_draw(VS_MARGIN_X, y, 0, tr, EPD_GFX_BLACK);
        y += 22;
    }
    for (int i = 0; i < VS_VISIBLE && i < s_cand_n; i++) {
        int ry = y + i * VS_ITEM_H;
        bool sel = (i == s_cand_sel);
        if (sel)
            epd_gfx_fill_rect(VS_MARGIN_X, ry, VS_ITEM_W - VS_SB_W - 4,
                              VS_ITEM_H - 4, EPD_GFX_BLACK);
        char line[WORD_TEXT_MAX + 70];
        snprintf(line, sizeof(line), "%s", s_cands[i].text);
        if (s_cands[i].meaning[0]) {
            char first[64];
            snprintf(first, sizeof(first), "%s", s_cands[i].meaning);
            char *nl = strchr(first, '\n');
            if (nl) *nl = '\0';
            size_t used = strlen(line);
            snprintf(line + used, sizeof(line) - used, "  %s", first);
        }
        cjk_text_draw_wrap(VS_MARGIN_X + 4, ry + (VS_ITEM_H - VS_FONT_H) / 2,
                           VS_ITEM_W - VS_SB_W - 12, VS_FONT_LVL,
                           VS_ITEM_H, 1, line,
                           sel ? EPD_GFX_WHITE : EPD_GFX_BLACK);
    }
}

/* 全刷整页（状态迁移；三色面板由头注降级规则不进本函数） */
static void vs_render_full(void)
{
    if (!epd_gfx_partial_supported()) return;   /* 三色零渲染（chat 先例） */
    epd_gfx_fill_screen(EPD_GFX_WHITE);
    vs_draw_title();
    switch (s_state) {
    case VOICE_IDLE:
        vs_draw_center("按中键说单词", s_hint);
        break;
    case VOICE_RECORDING:
        vs_draw_center("请说…", NULL);
        break;
    case VOICE_SEARCHING:
        vs_draw_center("识别中…", NULL);
        break;
    case VOICE_RESULT:
        vs_draw_result();
        break;
    default:
        break;
    }
    vs_draw_hint();
    vs_flush();
}

void voice_search_render(void)
{
    vs_render_full();
}

/* result 候选移动局刷（阈值保养；标题/transcript 行随列表区一并重画） */
static void vs_render_result_partial(void)
{
    if (!epd_gfx_partial_supported()) return;
    if (!refresh_gfx_before_partial_n(vs_partial_threshold())) {
        epd_gfx_fill_rect(0, VS_TITLE_H, epd_gfx_width(),
                          epd_gfx_height() - VS_TITLE_H, EPD_GFX_WHITE);
        vs_draw_result();
        vs_draw_hint();
        epd_gfx_flush_window_passes(0, VS_TITLE_H, epd_gfx_width(),
                                    epd_gfx_height() - VS_TITLE_H, 1);
        return;
    }
    vs_render_full();
}

/* ---- cloudId → 词库索引（当前词书内线性匹配；4000 词毫秒级） ---- */
static int cloud_id_to_index(const char *cloud_id)
{
    if (!cloud_id || !cloud_id[0]) return -1;
    int n = word_parser_get_count();
    for (int i = 0; i < n; i++) {
        const WordEntry *w = word_parser_get(i);
        if (w && strcmp(w->cloud_id, cloud_id) == 0) return i;
    }
    return -1;
}

/* ---- 上传（chat_upload 同构：multipart 三段流式 + 信封解析） ---- */

/* 按 URL 前缀选传输（chat_fill_cfg 同款第 6 处） */
static void vs_fill_cfg(esp_http_client_config_t *cfg, const char *url,
                        int timeout_ms)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->url = url;
    cfg->timeout_ms = timeout_ms;
    if (strncmp(url, "http:", 5) == 0) {
        cfg->transport_type = HTTP_TRANSPORT_OVER_TCP;
    } else {
        cfg->transport_type = HTTP_TRANSPORT_OVER_SSL;
        cfg->crt_bundle_attach = arduino_esp_crt_bundle_attach;
    }
}

static int vs_upload(const uint8_t *wav, size_t len)
{
    static const char BND[] = "InkWordVoice1886";
    static char resp[VS_RESP_MAX];

    char head[192], tail[48], url[224], ctype[64];
    int hl = snprintf(head, sizeof(head),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"vs.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n", BND);
    int fl = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", BND);
    snprintf(url, sizeof(url), "%s/api/device/voice-search?deck=%s",
             sync_get_base_url(), deck_manager_active_id());
    snprintf(ctype, sizeof(ctype),
             "multipart/form-data; boundary=%s", BND);

    esp_http_client_config_t cfg;
    vs_fill_cfg(&cfg, url, VS_UPLOAD_TIMEOUT_MS);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", ctype);
    esp_http_client_set_header(client, "X-Device-Key", sync_get_device_key());

    esp_err_t err = esp_http_client_open(client, hl + (int)len + fl);
    if (err == ESP_OK) {
        int w = esp_http_client_write(client, head, hl);
        if (w == hl) w = esp_http_client_write(client, (const char *)wav, (int)len);
        if (w == (int)len) w = esp_http_client_write(client, tail, fl);
        if (w != fl) err = ESP_FAIL;
    }

    int resp_len = 0;
    if (err == ESP_OK && esp_http_client_fetch_headers(client) >= 0)
        resp_len = esp_http_client_read(client, resp, sizeof(resp) - 1);

    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || resp_len <= 0) {
        LOG_W("voice upload failed: err=%d status=%d len=%d",
              err, status, resp_len);
        return -1;
    }
    resp[resp_len] = '\0';

    /* 信封 { code, data:{ transcript, candidates:[{text,meaning,cloudId}] } } */
    int rc = -1;
    s_cand_n = 0;
    s_transcript[0] = '\0';
    cJSON *root = cJSON_Parse(resp);
    if (root) {
        cJSON *code = cJSON_GetObjectItem(root, "code");
        cJSON *data = cJSON_GetObjectItem(root, "data");
        if (cJSON_IsNumber(code) && code->valueint == 0 && data) {
            cJSON *jtr = cJSON_GetObjectItem(data, "transcript");
            if (cJSON_IsString(jtr) && jtr->valuestring)
                snprintf(s_transcript, sizeof(s_transcript), "%s",
                         jtr->valuestring);
            cJSON *cands = cJSON_GetObjectItem(data, "candidates");
            if (cJSON_IsArray(cands)) {
                cJSON *jc;
                cJSON_ArrayForEach(jc, cands) {
                    if (s_cand_n >= VOICE_CAND_MAX) break;
                    cJSON *jt = cJSON_GetObjectItem(jc, "text");
                    cJSON *jm = cJSON_GetObjectItem(jc, "meaning");
                    cJSON *jci = cJSON_GetObjectItem(jc, "cloudId");
                    if (!cJSON_IsString(jt) || !jt->valuestring ||
                        !cJSON_IsString(jci) || !jci->valuestring) continue;
                    voice_cand_t *c = &s_cands[s_cand_n++];
                    snprintf(c->text, sizeof(c->text), "%s", jt->valuestring);
                    snprintf(c->cloud_id, sizeof(c->cloud_id), "%s",
                             jci->valuestring);
                    if (cJSON_IsString(jm) && jm->valuestring)
                        snprintf(c->meaning, sizeof(c->meaning), "%s",
                                 jm->valuestring);
                    else
                        c->meaning[0] = '\0';
                }
            }
            rc = 0;
        }
        cJSON_Delete(root);
    }
    if (rc != 0) LOG_W("voice payload invalid");
    return rc;
}

/* ---- 单轮：录音 → 上传 → 候选/直达 ---- */

static void set_hint(const char *msg)
{
    snprintf(s_hint, sizeof(s_hint), "%s", msg ? msg : "");
}

/* 候选跳词（cloudId 匹配当前词书；三色 top-1 与局刷选中共用） */
static void jump_candidate(int cand_idx)
{
    if (cand_idx < 0 || cand_idx >= s_cand_n) return;
    int idx = cloud_id_to_index(s_cands[cand_idx].cloud_id);
    if (idx < 0) {
        haptic_event(HAPTIC_ERROR);
        set_hint("不在当前词书 · 换词重试");
        s_state = VOICE_RESULT;    /* 回候选列表（局刷屏可见提示） */
        vs_render_full();
        return;
    }
    haptic_event(HAPTIC_MODE);
    study_mode_seek(idx);          /* 切 FLASH + 渲染词卡，本视图终结 */
    LOG_I("voice seek word #%d (%s)", idx, s_cands[cand_idx].text);
}

static void run_round(void)
{
    if (!wifi_is_connected()) {    /* 前置复检（enter 后网络可能变化） */
        haptic_event(HAPTIC_ERROR);
        set_hint("网络未连接");
        s_state = VOICE_IDLE;
        vs_render_full();
        return;
    }

    s_state = VOICE_RECORDING;
    set_hint(NULL);
    vs_render_full();
    haptic_event(HAPTIC_KEYPRESS);          /* 录音起一短震 */

    uint8_t *wav = NULL;
    size_t wav_len = 0;
    int rc = mic_recorder_record(&wav, &wav_len, &s_cancel, &s_send_now,
                                 VS_MAX_MS);
    if (!s_active) { free(wav); return; }   /* 模式已退：静默收尾 */
    if (rc == -2) {                          /* 用户取消：回 idle */
        s_state = VOICE_IDLE;
        vs_render_full();
        return;
    }
    if (rc != 0) {                           /* -3 无话音 / -1 失败 */
        haptic_event(rc == -3 ? HAPTIC_KEYPRESS : HAPTIC_ERROR);
        set_hint(rc == -3 ? "未听到 · 中键重试" : "录音失败 · 中键重试");
        s_state = VOICE_IDLE;
        vs_render_full();
        return;
    }

    s_state = VOICE_SEARCHING;
    vs_render_full();
    rc = vs_upload(wav, wav_len);
    free(wav);
    if (!s_active) return;
    if (rc != 0) {                           /* 网络/解析失败：一长震 */
        haptic_event(HAPTIC_ERROR);
        set_hint("网络失败 · 中键重试");
        s_state = VOICE_IDLE;
        vs_render_full();
        return;
    }
    if (s_cand_n == 0) {
        set_hint("未找到 · 中键重说");
        s_state = VOICE_IDLE;
        vs_render_full();
        return;
    }

    haptic_event(HAPTIC_PASS);               /* 候选到达两短震 */
    s_cand_sel = 0;

    if (!epd_gfx_partial_supported()) {      /* 三色降级：top-1 直达 */
        jump_candidate(0);
        return;
    }

    s_state = VOICE_RESULT;
    vs_render_full();
    LOG_I("voice result: %d cands, transcript=%.24s", s_cand_n, s_transcript);
}

static void voice_task(void *arg)
{
    (void)arg;
    while (s_active && !s_exit_req) {
        if (!s_round_req) {
            vTaskDelay(pdMS_TO_TICKS(20));   /* 轮询触发位（chat 同款） */
            continue;
        }
        s_round_req = false;
        run_round();
    }
    s_state = VOICE_IDLE;
    LOG_I("voice task exited");
    vTaskDelete(NULL);
}

/* ---- 公共 API ---- */

void voice_search_reset(void)
{
    if (s_active) return;                    /* 幂等（chat_mode_enter 同款） */
    s_active = true;
    s_exit_req = false;
    s_state = VOICE_IDLE;
    s_round_req = s_cancel = s_send_now = false;
    s_cand_n = s_cand_sel = 0;
    s_transcript[0] = s_hint[0] = '\0';
    if (xTaskCreate(voice_task, "voice", VS_TASK_STACK, NULL,
                    VS_TASK_PRIO, NULL) != pdPASS) {
        s_active = false;
        LOG_E("xTaskCreate voice failed");
    }
}

void voice_search_request_exit(void)
{
    s_exit_req = true;
    s_cancel = true;      /* 录音中退出：打断 mic_recorder 循环 */
    s_active = false;     /* 上传中退出：run_round 收尾静默（chat 同款） */
}

bool voice_search_is_active(void)
{
    return s_active;
}

voice_state_t voice_search_state(void)
{
    return s_state;
}

bool voice_search_on_button(nav_key_t id, button_event_t event)
{
    /* 退出请求：RST 长按 / 长按中（chat 编排同款，返回 false 由调用方执行） */
    if ((id == NAV_RST || id == NAV_CENTER) &&
        event == BUTTON_EVENT_LONG_PRESS)
        return false;

    if (event != BUTTON_EVENT_SHORT_PRESS) return true;   /* 其余长按忽略 */

    switch (s_state) {
    case VOICE_IDLE:
        if (id == NAV_CENTER) {
            s_cancel = false;
            s_send_now = false;
            s_round_req = true;    /* 任务起录（状态迁移由任务渲染） */
        }
        break;

    case VOICE_RECORDING:
        if (id == NAV_CENTER) s_send_now = true;   /* 提前收尾发送 */
        if (id == NAV_RST)    s_cancel = true;     /* 取消回 idle */
        break;

    case VOICE_SEARCHING:
        break;   /* 短暂（≤20s 超时兜底），短按忽略防误触 */

    case VOICE_RESULT:
        if (id == NAV_UP && s_cand_n > 0) {
            s_cand_sel = (s_cand_sel + s_cand_n - 1) % s_cand_n;
            vs_render_result_partial();
        } else if (id == NAV_DOWN && s_cand_n > 0) {
            s_cand_sel = (s_cand_sel + 1) % s_cand_n;
            vs_render_result_partial();
        } else if (id == NAV_CENTER) {
            jump_candidate(s_cand_sel);
        } else if (id == NAV_RST) {                 /* 回 idle 再查 */
            s_state = VOICE_IDLE;
            set_hint(NULL);
            vs_render_full();
        }
        break;

    default:
        break;
    }
    return true;
}
