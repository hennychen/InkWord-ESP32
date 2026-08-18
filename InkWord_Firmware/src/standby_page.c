/**
 * @file standby_page.c
 * @brief 无词库待机页实现（《传习录》引文独占：居中引文 + 右下出处）
 *
 * 布局（引文独占，GFX 横屏 416x240，rotation=1，用户 2026-08-18 定稿：
 * 仅显示《传习录》引文，星期/日期/农历/月年均不显示）：
 *   引文块 [112,8,192,176)：cjk_font 子集楷体 Bold 24px 点阵，
 *     8 字/行 x 5 行，行距 8px（行高 32，松排版）；水平居中
 *     （(416-192)/2），内容块带内垂直居中；
 *     每 STANDBY_QUOTE_INTERVAL_S(5min) 轮换一条（24 条循环）
 *   出处 [192,216)：右下角右对齐至 x=392 "——王阳明《传习录》"
 *     （k_chuanxilu_attrib，与引文同字库，静态不随小时变）
 *   时间无效时引文留白（仅出处）；天气不显示（数据链路保留，
 *   后端就绪后可随时加回）
 *
 * 刷新策略（2026-08-18 用户定稿：每 5 分钟轮换引文，一律全局刷新；
 * 局刷路径真机实测显示异常且有残影，弃用）：
 *   - 引文下标 = (epoch / STANDBY_QUOTE_INTERVAL_S(300s) + s_quote_off)
 *     % 24（自然窗口无状态派生，重启/校时自然对齐；s_quote_off 为
 *     短按 SET 手动轮换的相位偏移，重启归零）；下标变化即整页全局
 *     刷新（GxEPD2 display(false) 全刷模式写 previous，自带残影清理），
 *     自动轮换一天 288 次
 *   - 长按上：黑白交替深清 + 整页重绘（手动清陈年残影）
 *
 * 时间源（2026-08 真机实测后确定，应用层自治时钟，见 sb_time_start）：
 *   - 本机系统时钟路径已损坏：运营商劫持 UDP 123 令 SNTP 写入溢出垃圾值；
 *     禁用 SNTP 后 settimeofday 返回 0 但 time() 仍立即变成随机垃圾值
 *     （疑似预编译 esp_time 组件与当前 toolchain 深层不兼容），彻底弃用；
 *   - 自治钟：esp_timer 单调微秒为走时基准，HTTP Date 头（主）/ 后端
 *     serverTime（兜底，随天气投递）校时只更新 (epoch, timer) 基准对，
 *     读取时换算 UTC+8；未同步 30s 重试，成功后每 6h 防漂移校准。
 *     HTTP 请求由 sync_fetch_http_time() 发出（网络操作归 sync 模块）。
 */
#include "standby_page.h"
#include "epd_driver.h"
#include "refresh_scheduler.h"
#include "weather_icons.h"   /* 仅 WX_ICON_COUNT：NVS 缓存合法性校验（不绘制） */
#include "cjk_font.h"       /* 《传习录》引文楷体点阵 + 引文表 + 出处 */
#include "word_parser.h"
#include "wifi_manager.h"
#include "wifi_config_ui.h"
#include "lan_display_server.h"
#include "debug_log.h"

#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"

#include <time.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "STANDBY";

/* ---- 可配置项（可在 platformio.ini build_flags 覆盖） ---- */
#ifndef STANDBY_TZ
#define STANDBY_TZ              "CST-8"   /* POSIX 时区串：东八区 */
#endif
#ifndef STANDBY_QUOTE_INTERVAL_S
#define STANDBY_QUOTE_INTERVAL_S 300      /* 引文轮换间隔：5 分钟（全局刷新） */
#endif

/* ---- 布局常量（引文独占：居中引文 + 右下出处；y/h 均 8 对齐） ---- */
#define SB_QUOTE_X0       112    /* 引文带 x：(416-192)/2 水平居中 */
#define SB_QUOTE_Y0       8      /* 引文带 [8,184)：屏上部已无其他内容，满幅可用 */
#define SB_QUOTE_W        192    /* 24px 字格 x 8 列 = 192 */
#define SB_QUOTE_H        176    /* 带高（5 行×行高 32-尾距 8=152 + 余量；8 对齐） */
#define SB_QUOTE_LINE_GAP 8      /* 行间距：字格 24px 之外追加（松排版，不侵入出处带） */
#define SB_ATTR_Y0        192    /* 出处带 [192,216)：右下角 */
#define SB_ATTR_X1        392    /* 出处右缘（右边距 24 对齐） */
#define SB_VALID_UNIX     1735689600LL /* 2025-01-01 00:00:00 UTC，早于此未同步 */
#define SB_VALID_UNIX_MAX 4102444800LL /* 2100-01-01 00:00:00 UTC，晚于此被污染 */
#define SB_TZ_OFFSET_S    (8 * 3600)   /* 东八区，与 STANDBY_TZ 一致 */
#define SB_WX_CACHE_TTL_S (3 * 3600)   /* NVS 天气缓存有效期（数据链路，不绘制） */

/* HTTP Date 校时重试节奏（请求由 sync_fetch_http_time 发出，URL/超时
 * 归 sync_client.c）：运营商劫持 UDP 123 使 SNTP 不可用且会损坏系统时钟
 * （实测），80 端口 HTTP 几乎必通；未同步 30s 重试，成功后 6h 校准 */
#define SB_HTTP_RETRY_S     30     /* 未同步时重试间隔 */
#define SB_HTTP_CALIB_S     (6 * 3600) /* 已同步后防漂移校准间隔 */

/* ---- 模块状态 ---- */
static weather_info_t s_wx;                 /* 当前生效天气 */
static bool s_wx_valid = false;
static weather_info_t s_wx_pending;         /* 后台投递缓冲（写者->读者） */
static volatile bool s_wx_dirty = false;
static portMUX_TYPE s_wx_mux = portMUX_INITIALIZER_UNLOCKED;

static volatile bool s_flag_ghost_clear = false;   /* 长按上：loop 执行清残影 */
static volatile bool s_flag_fetch_weather = false; /* 短按中：loop 立即拉天气 */
static volatile bool s_flag_quote_next = false;    /* 短按 SET：手动轮换下一条引文 */
static int s_quote_off = 0;                        /* 手动轮换偏移（内存态，重启归零回自然对齐） */

static bool s_time_started = false;
static bool s_time_valid_announced = false; /* 时间同步成功只告一次（回退则重置） */
static int64_t s_http_try_us = 0;           /* 上次 HTTP 校时尝试时刻（esp_timer 单调微秒） */
static int64_t s_http_ok_us = 0;            /* 上次 HTTP 校时成功时刻 */
static int64_t s_time_epoch = -1;           /* 自治钟基准 Unix 秒；-1=未同步 */
static int64_t s_time_timer_us = 0;         /* 基准对应的 esp_timer 微秒 */

/* 上次绘制内容跟踪（tick 差异检测；校时强制置失效重画） */
static int s_last_quote = -2;         /* 引文下标（5 分钟窗；-1=无效空白，初始 -2 强制首绘） */

/* ============================================================
 * 应用层自治时钟：系统 time()/settimeofday 在本机损坏（settimeofday
 * 返回 0 但 time() 立即变随机垃圾值；SNTP 亦因运营商 UDP 123 劫持弃用，
 * 见文件头），以 esp_timer 单调钟换算 Unix 秒，UTC+8 纯日历转换。
 * ============================================================ */
static int64_t sb_epoch_now(void)
{
    if (s_time_epoch < 0) return -1;
    return s_time_epoch + (esp_timer_get_time() - s_time_timer_us) / 1000000;
}

/* 校时：无效 epoch 忽略；已同步且偏差 <60s 不重置（避免频繁重画时钟） */
static void sb_time_adjust(int64_t epoch)
{
    if (epoch < SB_VALID_UNIX || epoch > SB_VALID_UNIX_MAX) return;
    if (s_time_epoch > 0) {
        int64_t diff = sb_epoch_now() - epoch;
        if (diff < 60 && diff > -60) return;
    }
    s_time_epoch = epoch;
    s_time_timer_us = esp_timer_get_time();
    s_last_quote = -2;    /* 校时后强制重画引文（5 分钟窗可能变化） */
}

/* ============================================================
 * NVS 天气缓存：重启即有画面（超过 TTL 降级 "--"）
 * ============================================================ */
static void sb_wx_save(const weather_info_t *w)
{
    nvs_handle_t h;
    if (nvs_open("inkword", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "wx_cache", w, sizeof(*w));
    /* 拉取时刻作为缓存时间戳；时钟未同步时存 0（下次启动无法判龄则直接采用） */
    int64_t now = sb_epoch_now();
    nvs_set_u32(h, "wx_ts", (now >= SB_VALID_UNIX) ? (uint32_t)now : 0);
    nvs_commit(h);
    nvs_close(h);
}

static void sb_wx_load(void)
{
    nvs_handle_t h;
    if (nvs_open("inkword", NVS_READONLY, &h) != ESP_OK) return;

    weather_info_t w;
    size_t len = sizeof(w);
    uint32_t ts = 0;
    if (nvs_get_blob(h, "wx_cache", &w, &len) == ESP_OK && len == sizeof(w) &&
        nvs_get_u32(h, "wx_ts", &ts) == ESP_OK && w.icon < WX_ICON_COUNT) {
        bool fresh = true;
        int64_t now = sb_epoch_now();
        if (now >= SB_VALID_UNIX && ts != 0 &&
            (uint64_t)now > (uint64_t)ts + SB_WX_CACHE_TTL_S) {
            fresh = false;   /* 缓存超时：开机后由首次联网拉取覆盖 */
        }
        if (fresh) {
            s_wx = w;
            s_wx_valid = true;
        }
    }
    nvs_close(h);
}

/* ============================================================
 * 引文 / 出处绘制（引文独占布局，用户 2026-08-18 定稿）
 * ============================================================ */
/* UTF-8 前向解码：返回码点，*len 吃进的字节数（引文全部 BMP 三字节） */
static uint32_t sb_utf8_next(const char *s, int *len)
{
    unsigned char c = (unsigned char)s[0];
    if ((c & 0x80) == 0) { *len = 1; return c; }
    if ((c & 0xE0) == 0xC0) { *len = 2; return ((c & 0x1F) << 6) | (s[1] & 0x3F); }
    if ((c & 0xF0) == 0xE0) { *len = 3; return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); }
    *len = 1;
    return c;
}

/* 《传习录》引文：楷体 24px 点阵，8 字/行，行距 SB_QUOTE_LINE_GAP，
 * 块垂直居中；idx<0 留白 */
static void sb_draw_quote(int idx)
{
    if (idx < 0 || idx >= CHUANXILU_QUOTE_N) return;
    const char *q = k_chuanxilu_quotes[idx];

    int lines = 1;
    for (const char *p = q; *p; p++)
        if (*p == '\n') lines++;

    int line_h = CJK_GLYPH_H + SB_QUOTE_LINE_GAP;   /* 行高 = 字格 + 行距 */
    int block_h = lines * line_h - SB_QUOTE_LINE_GAP; /* 末行不带尾距 */
    int y = SB_QUOTE_Y0 + (SB_QUOTE_H - block_h) / 2;
    int row = 0, col = 0, i = 0;
    while (q[i]) {
        if (q[i] == '\n') { row++; col = 0; i++; continue; }
        int len;
        uint32_t cp = sb_utf8_next(&q[i], &len);
        const uint8_t *bits = cjk_glyph_lookup(cp);
        if (bits)
            epd_gfx_draw_bitmap(SB_QUOTE_X0 + col * CJK_GLYPH_W,
                                y + row * line_h,
                                CJK_GLYPH_W, CJK_GLYPH_H, bits, EPD_GFX_BLACK);
        col++;
        i += len;
    }
}

/* 出处：右下角右对齐 "——王阳明《传习录》"（与引文同字库，静态） */
static void sb_draw_attrib(void)
{
    int n = 0;
    for (const char *p = k_chuanxilu_attrib; *p; ) {
        int len;
        sb_utf8_next(p, &len);
        p += len;
        n++;
    }
    int x = SB_ATTR_X1 - n * CJK_GLYPH_W;
    int col = 0, i = 0;
    while (k_chuanxilu_attrib[i]) {
        int len;
        uint32_t cp = sb_utf8_next(&k_chuanxilu_attrib[i], &len);
        const uint8_t *bits = cjk_glyph_lookup(cp);
        if (bits)
            epd_gfx_draw_bitmap(x + col * CJK_GLYPH_W, SB_ATTR_Y0,
                                CJK_GLYPH_W, CJK_GLYPH_H, bits, EPD_GFX_BLACK);
        col++;
        i += len;
    }
}

/* ============================================================
 * 当前内容计算（时间无效 -> 引文留白，仅出处）
 * ============================================================ */
/* 当前本地时间（UTC+8；gmtime_r 纯日历算法，不依赖系统时钟/TZ 环境） */
static bool sb_now(struct tm *out_tm)
{
    int64_t sec = sb_epoch_now();
    if (sec < SB_VALID_UNIX || sec > SB_VALID_UNIX_MAX) {
        memset(out_tm, 0, sizeof(*out_tm));
        return false;
    }
    time_t t = (time_t)(sec + SB_TZ_OFFSET_S);
    gmtime_r(&t, out_tm);
    return true;
}

/* 当前引文下标：epoch 按 STANDBY_QUOTE_INTERVAL_S 分窗取模 24 条
 * （无状态派生，重启/校时自然对齐同一窗口；时间无效返回 -1 留白）。
 * s_quote_off 为 SET 手动轮换偏移：叠加在自然窗口之上，
 * 下标变化由 tick 差异检测捕获并整页刷新 */
static int sb_quote_now(void)
{
    int64_t sec = sb_epoch_now();
    if (sec < SB_VALID_UNIX || sec > SB_VALID_UNIX_MAX) return -1;
    return (int)(((sec / STANDBY_QUOTE_INTERVAL_S) + s_quote_off)
                 % CHUANXILU_QUOTE_N);
}

static void sb_time_start(void)
{
    /* 系统时钟路径（SNTP/settimeofday/time）已彻底弃用，原因见文件头与
     * 自治钟注释；此处仅置标志，校时由 tick 内 HTTP Date 触发 */
    s_time_started = true;
    LOG_I("time service started (tz=%s, esp_timer clock, HTTP Date mode)",
          STANDBY_TZ);
}

/* ============================================================
 * 公共 API
 * ============================================================ */
void standby_init(void)
{
    memset(&s_wx, 0, sizeof(s_wx));
    s_wx_valid = false;
    sb_wx_load();
    LOG_I("standby page ready (weather cache %s)", s_wx_valid ? "hit" : "miss");
}

bool standby_is_active(void)
{
    return word_parser_get_count() == 0;
}

void standby_weather_update(const weather_info_t *w)
{
    if (!w) return;
    portENTER_CRITICAL(&s_wx_mux);
    s_wx_pending = *w;       /* 短临界区拷贝，读者在主循环取走 */
    s_wx_dirty = true;
    portEXIT_CRITICAL(&s_wx_mux);
}

void standby_on_button(nav_key_t id, button_event_t event)
{
    if (event == BUTTON_EVENT_LONG_PRESS) {
        switch (id) {
        case NAV_CENTER:
            wifi_config_ui_enter();          /* 既有跨任务进入模式 */
            break;
        case NAV_UP:
            s_flag_ghost_clear = true;       /* loop 中执行清残影+整页重绘 */
            break;
        case NAV_LEFT:
            lan_portal_enter();
            break;
        case NAV_RIGHT:
            lan_server_enter_receive_page();
            break;
        default:
            break;
        }
        return;
    }

    /* 短按中 = 立即拉取天气（loop 中联网执行）；短按 SET = 手动轮换
     * 下一条引文（不用等 5 分钟窗）；其余短按忽略：
     * 无词库无内容可翻，避免烧刷新次数 */
    if (event == BUTTON_EVENT_SHORT_PRESS && id == NAV_CENTER) {
        s_flag_fetch_weather = true;
    }
    if (event == BUTTON_EVENT_SHORT_PRESS && id == NAV_SET) {
        s_flag_quote_next = true;
    }
}

void standby_render_full(void)
{
    if (!standby_is_active()) return;
    if (wifi_config_ui_is_active() || lan_server_is_active()) return;

    int quote = sb_quote_now();

    epd_gfx_fill_screen(EPD_GFX_WHITE);
    sb_draw_quote(quote);
    sb_draw_attrib();
    epd_gfx_flush();   /* 整页全刷 */

    s_last_quote = quote;    /* 同步跟踪状态，避免 tick 误判首帧差异 */

    LOG_I("standby page rendered: quote=%d (%s)", quote,
          quote >= 0 ? k_chuanxilu_quotes[quote] : "time not synced");
}

void standby_tick(void)
{
    if (!standby_is_active()) return;

    /* ---- 按键置位的动作（须在本主循环上下文执行） ---- */
    if (s_flag_fetch_weather) {
        s_flag_fetch_weather = false;
        if (wifi_is_connected()) {
            weather_info_t wx;
            if (sync_fetch_weather(&wx) == 0) {
                standby_weather_update(&wx);
            } else {
                LOG_W("manual weather fetch failed");
            }
        } else {
            LOG_W("weather fetch skipped: wifi not connected");
        }
    }
    if (s_flag_ghost_clear) {
        s_flag_ghost_clear = false;
        if (!wifi_config_ui_is_active() && !lan_server_is_active()) {
            refresh_force_full();        /* 清屏全刷 + 局刷计数归零 */
            standby_render_full();       /* 整页重绘 + 全刷 */
        }
    }
    if (s_flag_quote_next) {
        s_flag_quote_next = false;
        s_quote_off++;                  /* 下标变化交由 tick 差异检测整页刷新 */
    }

    /* 配网页 / LAN 接收页接管屏幕期间不绘制 */
    if (wifi_config_ui_is_active() || lan_server_is_active()) return;

    /* ---- 消费后台投递的天气（页面已不绘制；仅 NVS 持久化 + 校时兜底） ---- */
    if (s_wx_dirty) {
        weather_info_t wx;
        portENTER_CRITICAL(&s_wx_mux);
        wx = s_wx_pending;
        s_wx_dirty = false;
        portEXIT_CRITICAL(&s_wx_mux);

        s_wx = wx;
        s_wx_valid = true;
        sb_wx_save(&wx);

        if (wx.server_time >= SB_VALID_UNIX && wx.server_time <= SB_VALID_UNIX_MAX) {
            int64_t before = sb_epoch_now();
            sb_time_adjust(wx.server_time);
            if (sb_epoch_now() != before)
                LOG_I("clock adjusted from server time (diff=%llds)",
                      (long long)(before - wx.server_time));
        }
    }

    /* ---- 时间服务：联网即启动，HTTP Date 为主源（30s 重试 / 6h 校准） ---- */
    struct tm tm_;
    bool valid = sb_now(&tm_);
    if (wifi_is_connected()) {
        if (!s_time_started) {
            sb_time_start();
        }
        int64_t now_us = esp_timer_get_time();
        bool due = s_http_try_us == 0 ||
                   (!valid && now_us - s_http_try_us >= (int64_t)SB_HTTP_RETRY_S * 1000000) ||
                   (valid && s_http_ok_us > 0 &&
                    now_us - s_http_ok_us >= (int64_t)SB_HTTP_CALIB_S * 1000000);
        if (due) {
            s_http_try_us = now_us;
            int64_t t = sync_fetch_http_time();  /* 一次性阻塞 ≤5s，仅低频触发 */
            if (t > 0) {
                sb_time_adjust(t);
                s_http_ok_us = now_us;
            }
        }
    }

    /* 时间同步成功（或丢失）只告一次，串口可观测 HTTP 校时是否生效 */
    if (valid && !s_time_valid_announced) {
        s_time_valid_announced = true;
        LOG_I("time synced: %04d-%02d-%02d %02d:%02d:%02d",
              tm_.tm_year + 1900, tm_.tm_mon + 1, tm_.tm_mday,
              tm_.tm_hour, tm_.tm_min, tm_.tm_sec);
    } else if (!valid && s_time_valid_announced) {
        s_time_valid_announced = false;   /* 时间回退，重新等待同步 */
    }

    /* ---- 引文轮换：5 分钟窗下标变化即整页全局刷新（无局刷路径） ---- */
    int quote = sb_quote_now();
    if (quote == s_last_quote) return;

    standby_render_full();   /* 整页重绘 + 全刷（内容日志见 rendered） */
}
