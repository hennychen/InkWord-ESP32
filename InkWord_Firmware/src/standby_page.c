/**
 * @file standby_page.c
 * @brief 无词库待机页实现（《传习录》引文独占：居中引文 + 右下出处）
 *
 * 布局（引文独占，GFX 横屏 416x240，rotation=1，用户 2026-08-18 定稿：
 * 仅显示《传习录》引文，星期/日期/农历/月年均不显示）：
 *   引文块 [112,8,192,176)：cjk_font 子集楷体 Bold 点阵（档位字库级，
 *     MID 档 24px 与历史定稿一致；SMALL 档 16px），
 *     8 字/行 x 5 行，行距 8px（行高 32，松排版）；水平居中
 *     （(416-192)/2），内容块带内垂直居中；
 *     每 STANDBY_QUOTE_INTERVAL_S(5min) 轮换一条（24 条循环）
 *   出处 [192,216)：右下角右对齐至 x=392 "——王阳明《传习录》"
 *     （k_chuanxilu_attrib，与引文同字库，静态不随小时变）
 *   时间无效时引文留白（仅出处）；天气不显示（数据链路保留，
 *   后端就绪后可随时加回）
 *
 * 刷新策略（2026-08-20：引文轮换单段直接差分局刷 + 智能分流全刷，
 * 见 epd_driver.cpp）：
 *   - 引文下标 = (epoch / STANDBY_QUOTE_INTERVAL_S(300s) + s_quote_off)
 *     % 24（自然窗口无状态派生，重启/校时自然对齐；s_quote_off 为
 *     短按 SET 手动轮换的相位偏移，重启归零）；下标变化时：
 *       a) 差分预检分流：新帧与屏幕影子差分 > 带面积 25%（大面积
 *          变化）或局刷计数达阈值 → 全刷；否则单段直接差分局刷；
 *       b) 单段直接差分：画布绘完整新帧（旧字位白+新字位黑）后
 *          一次局刷，COG 双 RAM 差分同 pass 驱动两方向翻转像素，
 *          波形 387ms、切换 ≈0.5s（真机验证无残影定稿；完整方案
 *          与规则见 README「局部刷新方案」节）；
 *       c) 首绘/校时跳变（s_last_quote==-2）：无影子基准，直接全刷
 *   - 低频保养（STANDBY_PARTIAL_MAX_N=12）：无窗口双 RAM 局刷自身
 *     无残影（2026-08-20 真机验证），真全刷降级为例行深度保养，
 *     自动轮换下 ≈ 1 小时一次；真全刷波形黑白交替闪烁属正常视觉，
 *     连续手动 SET 翻 12 次才会遇到一次
 *   - 引文带 [112,8,192,176)：无窗口整屏差分路径，无 8 对齐约束
 *     （窗口对齐要求随窗口模式一并弃用）；
 *   - 前帧缓存：epd_driver 自持 s_port_prev 双帧（每次刷新后同步），
 *     局刷 0x10 差分基准始终与屏幕真实内容一致；
 *   - 三色面板（partial 不可用，§13.2）：自动轮换停用 —— 自然窗
 *     冻结在最近渲染窗（s_quote_hold_win），仅 SET 手动翻页可用；
 *     渲染路径局刷自动降级整帧全刷（epd_driver flush_window*），
 *     SET 翻页 = 一次 ~16s 全刷（用户主动，计费可接受）；
 *   - 电源：刷新尾 0x02 Power OFF（关高压 rail、保 VCI 逻辑供电），
 *     从不 Deep Sleep —— 控制器状态保留路径与局刷兼容；
 *     局刷前硬复位安全：demo 双 RAM 写入不依赖 COG 内部缓存存活
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
#include "layout_profile.h" /* Phase 5：档位→字库级映射（引文大字场景） */
#include "word_parser.h"
#include "study_mode_machine.h"  /* 阅读模式书页接管屏幕时待机页退位 */
#include "wifi_manager.h"
#include "wifi_config_ui.h"
#include "lan_display_server.h"
#include "menu_ui.h"      /* 快捷菜单接管屏幕期间待机页退位（长按中进入） */
#include "debug_log.h"

#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"

#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>   /* Phase 5：影子/抓取缓冲运行期分配（standby_init） */

static const char *TAG = "STANDBY";

/* ---- 可配置项（可在 platformio.ini build_flags 覆盖） ---- */
#ifndef STANDBY_TZ
#define STANDBY_TZ              "CST-8"   /* POSIX 时区串：东八区 */
#endif
#ifndef STANDBY_QUOTE_INTERVAL_S
#define STANDBY_QUOTE_INTERVAL_S 300      /* 引文轮换间隔：5 分钟 */
#endif
#ifndef STANDBY_PARTIAL_MAX_DIFF_PX
#define STANDBY_PARTIAL_MAX_DIFF_PX (-1)  /* 差分像素阈值：<0 = 运行期按带面积 25%（Phase 5 随带尺寸参数化） */
#endif
#ifndef STANDBY_PARTIAL_MAX_N
#define STANDBY_PARTIAL_MAX_N 12          /* 连续局刷次数上限 → 低频真全刷深度
                                           * 保养（无窗口双 RAM 局刷已无残影，
                                           * 全刷从窗口时代的频繁清洗降级为
                                           * 例行保养，自动轮换下 ≈ 1 小时一次） */
#endif

/* ---- 布局常量（引文独占：居中引文 + 右下出处） ----
 * Phase 5 档位化：字格尺寸运行期取布局档位字库级（MID=24px 与旧
 * 字面精确相等，视觉零变化；SMALL=16px），带宽/带高/坐标全部由
 * 字格 s_cell 派生（MID 416x240 括号值逐项等于旧宏）：
 *   quote_w  = SB_QUOTE_COLS * cell                （192）
 *   x0       = (gfx_w - quote_w) / 2                （112）
 *   attr_y0  = gfx_h - SB_ATTR_BOTTOM - cell        （192）
 *   quote_h  = attr_y0 - SB_ATTR_GAP - SB_QUOTE_Y0  （176）
 * 影子/抓取缓冲按运行期带尺寸堆分配（standby_init），static 数组
 * 尺寸约束解除（Phase 4 遗留注释同步作废） */
#define SB_QUOTE_COLS      8     /* 每行字数上限（引文数据约束 <=8 字/行） */
/* 布局常量 SMALL 档紧化（2026-08-22 升 24px 配套）：2.7" 176px 短边下
 * 五行引文块 128px + 出处带 36px 需带高 ≥132px，原 MID 参数（8/8/8/24）
 * 仅 112px 装不下。三元取值：MID/LARGE 保持原值（视觉零变化铁律）；
 * 2026-08-23 TINY 档（引文 16px）同走紧化分支 */
#define SB_QUOTE_Y0        (s_tight ? 4 : 8)   /* 引文带顶 */
#define SB_QUOTE_LINE_GAP  (s_tight ? 2 : 8)   /* 行间距：字格之外追加 */
#define SB_ATTR_BOTTOM     (s_tight ? 12 : 24) /* 出处底边距 */
#define SB_ATTR_GAP        (s_tight ? 4 : 8)   /* 引文带底与出处带顶间隙 */
#define SB_QUOTE_W         (SB_QUOTE_COLS * s_cell)
/* 引文带左缘：TINY 竖屏 122/128px 宽 < 8 字×16px=128px 带宽时贴左缘
 * （零或负居中值钳 0；极宽 8 字行右缘可贴边甚至溢出 ≤6px，真实引文
 * 多数 ≤6 字/行，bring-up 实测后再调列宽或缩字） */
#define SB_QUOTE_X0        (epd_gfx_width() > SB_QUOTE_W \
                             ? (epd_gfx_width() - SB_QUOTE_W) / 2 : 0)
#define SB_ATTR_Y0         (epd_gfx_height() - SB_ATTR_BOTTOM - s_cell)
#define SB_QUOTE_H         (SB_ATTR_Y0 - SB_ATTR_GAP - SB_QUOTE_Y0)
#define SB_ATTR_X1         (epd_gfx_width() - 24)  /* 出处右缘：右边距 24（416→392） */
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
static int s_quote_hold_win = -1;                  /* 三色屏自然窗冻结（-1=未冻结，首绘用实时窗；
                                                     * BW 面板 partial 可用恒不参与，§13.2） */

static bool s_time_started = false;
static bool s_time_valid_announced = false; /* 时间同步成功只告一次（回退则重置） */
static int64_t s_http_try_us = 0;           /* 上次 HTTP 校时尝试时刻（esp_timer 单调微秒） */
static int64_t s_http_ok_us = 0;            /* 上次 HTTP 校时成功时刻 */
static int64_t s_time_epoch = -1;           /* 自治钟基准 Unix 秒；-1=未同步 */
static int64_t s_time_timer_us = 0;         /* 基准对应的 esp_timer 微秒 */

/* 上次绘制内容跟踪（tick 差异检测；校时强制置失效重画） */
static int s_last_quote = -2;         /* 引文下标（5 分钟窗；-1=无效空白，初始 -2 强制首绘） */

/* Phase 5 档位化：字库级与字格（standby_init 按布局档位填充；
 * MID 档 level=2 即 24px，与旧 CJK_GLYPH_W/H 硬宏精确相等。
 * 初值为 epd 未初始化前的兜底，绘制前必经 standby_init 覆盖） */
static int s_quote_level = 2;
static int s_cell = CJK_GLYPH_H;
static bool s_tight = false;  /* SMALL/TINY 档紧排版（standby_init 置位）：
                               * 176px 短边容纳 24px 五行引文需压行距/边距
                               *（见下方布局常量三元分支，MID/LARGE 原值） */

/* 引文带影子缓存（局刷智能分流的差分基准；Phase 5 改运行期按带
 * 尺寸堆分配，LARGE 档带高增长不再受编译期上限约束）：
 * s_quote_shadow = 屏幕当前引文带快照（draw_bitmap 同格式，bit=1=黑）；
 * s_quote_scratch = 新帧读回缓冲。首绘/全刷后同步，局刷前差分；
 * 分配失败时 s_shadow_valid 恒 false，轮换安全降级为全刷 */
static uint8_t *s_quote_shadow = NULL;
static uint8_t *s_quote_scratch = NULL;
static int s_quote_bytes = 0;
static bool s_shadow_valid = false;   /* 影子是否可信（首绘前不可信） */

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

/* 公共导出（.h 声明）：learning_state 今日统计日结判定用。
 * 未同步返回 -1，调用方挂起日结（计数不推进，同步后首个评分补结） */
int64_t standby_time_now(void)
{
    return sb_epoch_now();
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

/* 《传习录》引文：楷体点阵（档位字库级，MID=24px），8 字/行，
 * 行距 SB_QUOTE_LINE_GAP，块垂直居中；idx<0 留白 */
static void sb_draw_quote(int idx)
{
    if (idx < 0 || idx >= CHUANXILU_QUOTE_N) return;
    const char *q = k_chuanxilu_quotes[idx];

    int lines = 1;
    for (const char *p = q; *p; p++)
        if (*p == '\n') lines++;

    int line_h = s_cell + SB_QUOTE_LINE_GAP;     /* 行高 = 字格 + 行距 */
    int block_h = lines * line_h - SB_QUOTE_LINE_GAP; /* 末行不带尾距 */
    int y = SB_QUOTE_Y0 + (SB_QUOTE_H - block_h) / 2;
    int row = 0, col = 0, i = 0;
    while (q[i]) {
        if (q[i] == '\n') { row++; col = 0; i++; continue; }
        int len;
        uint32_t cp = sb_utf8_next(&q[i], &len);
        const uint8_t *bits = cjk_glyph_lookup_level(cp, s_quote_level);
        if (bits)
            epd_gfx_draw_bitmap(SB_QUOTE_X0 + col * s_cell,
                                y + row * line_h,
                                s_cell, s_cell, bits, EPD_GFX_BLACK);
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
    int x = SB_ATTR_X1 - n * s_cell;
    int col = 0, i = 0;
    while (k_chuanxilu_attrib[i]) {
        int len;
        uint32_t cp = sb_utf8_next(&k_chuanxilu_attrib[i], &len);
        const uint8_t *bits = cjk_glyph_lookup_level(cp, s_quote_level);
        if (bits)
            epd_gfx_draw_bitmap(x + col * s_cell, SB_ATTR_Y0,
                                s_cell, s_cell, bits, EPD_GFX_BLACK);
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
/* 三色屏自动轮换冻结窗同步：每次实际渲染后记录当前自然窗，
 * sb_quote_now 以冻结窗替代实时窗（时间窗变化不再触发刷新，
 * SET 偏移仍可翻页）。BW 面板 partial 可用，冻结窗不启用 */
static void sb_hold_win_sync(void)
{
    if (epd_gfx_partial_supported()) return;
    int64_t sec = sb_epoch_now();
    if (sec >= SB_VALID_UNIX && sec <= SB_VALID_UNIX_MAX)
        s_quote_hold_win = (int)(sec / STANDBY_QUOTE_INTERVAL_S);
}

static int sb_quote_now(void)
{
    int64_t sec = sb_epoch_now();
    if (sec < SB_VALID_UNIX || sec > SB_VALID_UNIX_MAX) return -1;
    int64_t win = sec / STANDBY_QUOTE_INTERVAL_S;
    /* 三色屏（无快速局刷，全刷 ~16s，§13.2）：自动轮换停用 ——
     * 自然窗冻结在最近一次渲染的窗，仅 SET 手动偏移可翻页 */
    if (!epd_gfx_partial_supported() && s_quote_hold_win >= 0)
        win = s_quote_hold_win;
    return (int)((win + s_quote_off) % CHUANXILU_QUOTE_N);
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
 * P5 深睡时钟交接：NVS 基准对 + RTC 慢钟差分
 * esp_timer 随 SoC 深睡归零，自治钟基准对不能直接存活；系统 time()
 * 绝对值不可信（settimeofday 路径损坏）但其在深睡中由 RTC 慢钟维持
 * 走时且跨深睡连续 —— 只取差分（time(NULL) - 入睡时刻值）无碰损坏
 * 路径。冷启动 RTC 清零，差分无意义，restore 仅限深睡唤醒后的启动
 * 路径调用（main.cpp 按唤醒原因分流）。
 * 精度：内部 RC 慢钟小时级睡眠误差分钟级，联网后 HTTP Date 校准兜底
 * ============================================================ */
#define SB_NVS_SLEEP_EPOCH  "slp_ep0"   /* 入睡时刻自治钟基准 Unix 秒 */
#define SB_NVS_SLEEP_RTC    "slp_rtc0"  /* 入睡时刻系统 RTC 原始值（仅取差分） */

void standby_time_checkpoint(void)
{
    /* 时间未同步：不写基准对（唤醒后维持未同步留白，联网走 HTTP 校时） */
    if (s_time_epoch < SB_VALID_UNIX) return;

    nvs_handle_t h;
    if (nvs_open("inkword", NVS_READWRITE, &h) != ESP_OK) return;
    int64_t rtc0 = (int64_t)time(NULL);
    nvs_set_i64(h, SB_NVS_SLEEP_EPOCH, s_time_epoch);
    nvs_set_i64(h, SB_NVS_SLEEP_RTC, rtc0);
    nvs_commit(h);
    nvs_close(h);
    LOG_I("clock checkpoint: epoch=%lld rtc0=%lld",
          (long long)s_time_epoch, (long long)rtc0);
}

void standby_time_restore(void)
{
    nvs_handle_t h;
    if (nvs_open("inkword", NVS_READONLY, &h) != ESP_OK) return;

    int64_t ep0 = 0, rtc0 = 0;
    bool ok = nvs_get_i64(h, SB_NVS_SLEEP_EPOCH, &ep0) == ESP_OK &&
              nvs_get_i64(h, SB_NVS_SLEEP_RTC, &rtc0) == ESP_OK;
    nvs_close(h);
    if (!ok || ep0 < SB_VALID_UNIX) return;

    int64_t rtc_diff = (int64_t)time(NULL) - rtc0;
    if (rtc_diff < 0) rtc_diff = 0;  /* RTC 异常倒退：按 0 保守处理 */

    /* 重建基准对：esp_timer 已归零，以当前时刻为新计时起点 */
    s_time_epoch = ep0 + rtc_diff;
    s_time_timer_us = esp_timer_get_time();
    s_time_started = true;
    s_last_quote = -2;   /* 首帧强制重判（5 分钟窗可能已切换） */
    LOG_I("clock restored: rtc_diff=%llds epoch=%lld",
          (long long)rtc_diff, (long long)s_time_epoch);
}

void standby_time_set(int64_t epoch)
{
    sb_time_adjust(epoch);   /* 复用校时：无效区间忽略，<60s 漂移不重置 */
}

/* ============================================================
 * 公共 API
 * ============================================================ */
void standby_init(void)
{
    memset(&s_wx, 0, sizeof(s_wx));
    s_wx_valid = false;
    sb_wx_load();

    /* Phase 5 档位化：按布局档位取引文字库级（SMALL=16px / MID=24px），
     * 按带尺寸重配影子/抓取缓冲（重启/深睡唤醒重入幂等；epd 几何
     * 已于 epd_driver_init 就绪，且首调 layout_profile_get 缓存档位） */
    s_quote_level = layout_profile_get()->quote_level;
    s_cell = cjk_glyph_cell_size(s_quote_level);
    s_tight = (layout_profile_get()->kind <= LAYOUT_SMALL);  /* 含 TINY */
    int bytes = SB_QUOTE_W / 8 * SB_QUOTE_H;
    if (bytes != s_quote_bytes || !s_quote_shadow || !s_quote_scratch) {
        free(s_quote_shadow);
        free(s_quote_scratch);
        s_quote_shadow = (uint8_t *)malloc(bytes);
        s_quote_scratch = (uint8_t *)malloc(bytes);
        if (s_quote_shadow && s_quote_scratch) {
            s_quote_bytes = bytes;
        } else {   /* 分配失败：差分预检降级，轮换恒走全刷（功能不损） */
            free(s_quote_shadow); s_quote_shadow = NULL;
            free(s_quote_scratch); s_quote_scratch = NULL;
            s_quote_bytes = 0;
            LOG_W("quote shadow alloc %dB failed, rotate degrades to full",
                  bytes);
        }
    }
    s_shadow_valid = false;   /* 缓冲重建/首绘前影子不可信 */

    LOG_I("standby page ready (weather cache %s, quote level=%d %dpx)",
          s_wx_valid ? "hit" : "miss", s_quote_level, s_cell);
    if (!epd_gfx_partial_supported())
        LOG_I("no fast partial: quote auto-rotate disabled (SET manual only)");
}

bool standby_is_active(void)
{
    /* 阅读模式（P3）书页/占位页接管屏幕：待机页（含分钟心跳整刷）
     * 退位，避免词库空时读书被待机页周期性冲掉 */
    return word_parser_get_count() == 0 &&
           study_mode_current() != MODE_READER;
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
            menu_ui_enter();             /* 既有跨任务进入模式（原 Wi-Fi 配网降为菜单项） */
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
    if (wifi_config_ui_is_active() || lan_server_is_active() ||
        menu_ui_is_active()) return;

    int quote = sb_quote_now();

    epd_gfx_fill_screen(EPD_GFX_WHITE);
    sb_draw_quote(quote);
    sb_draw_attrib();
    epd_gfx_flush();   /* 整页全刷 */

    s_last_quote = quote;    /* 同步跟踪状态，避免 tick 误判首帧差异 */
    sb_hold_win_sync();    /* 三色屏：冻结自然窗（自动轮换停用） */

    /* 全刷后屏幕与画布同步，刷新引文带影子（局刷差分新基准；
     * 缓冲分配失败时保持 invalid，轮换恒走全刷） */
    if (s_quote_shadow) {
        epd_gfx_read_window(SB_QUOTE_X0, SB_QUOTE_Y0, SB_QUOTE_W, SB_QUOTE_H,
                            s_quote_shadow);
        s_shadow_valid = true;
    }

    LOG_I("standby page rendered: quote=%d (%s)", quote,
          quote >= 0 ? k_chuanxilu_quotes[quote] : "time not synced");
}

/* 引文轮换绘制：单段直接差分局刷 + 差分智能分流。
 * 仅在已有屏幕基准（s_last_quote >= -1 且影子可信）时由 tick 调用；
 * 首绘/校时跳变走 standby_render_full（全刷）。
 *
 * 单段直接差分（2026-08-20 方案 B，取代两段式）：预检时画布已绘好
 * 完整新帧（旧字位白 + 新字位黑），一次局刷让 COG 双 RAM 差分在
 * 同一 pass 驱动两方向翻转像素，波形时间 387ms（两段式 774ms 减半，
 * 切换约 0.5s）。
 * 真机验证（2026-08-20）：无残影，定稿（方案演进与规则见 README
 * 「局部刷新方案」节）。理论依据：方案 A 已验证无窗口波形下
 * 黑→白单刷即净（该方向驱动力充足，窗口模式“混合差分留浅影”
 * 的主因已排除）。
 * 回退指引（若真机出现旧字浅影，即同 pass 混合方向翻不彻底）：
 *   1) epd_gfx_fill_rect(带, 白) + flush_window_passes(带, 1)  洗旧字
 *   2) sb_draw_quote(quote) + flush_window_passes(带, 1)      绘新字
 * 两段式波形 774ms、真机验证无残影（完整代码见 git 历史或记忆） */
static void standby_render_quote(void)
{
    int quote = sb_quote_now();

    /* 0. 差分预检：先在画布绘好新帧，读回与屏幕影子比较分流 */
    epd_gfx_fill_rect(SB_QUOTE_X0, SB_QUOTE_Y0, SB_QUOTE_W, SB_QUOTE_H,
                      EPD_GFX_WHITE);
    sb_draw_quote(quote);
    epd_gfx_read_window(SB_QUOTE_X0, SB_QUOTE_Y0, SB_QUOTE_W, SB_QUOTE_H,
                        s_quote_scratch);
    int diff_px = 0;
    for (int i = 0; i < s_quote_bytes; i++)
        diff_px += __builtin_popcount(s_quote_shadow[i] ^ s_quote_scratch[i]);

    /* 智能分流：大面积变化（差分超阈值）或局刷计数达阈值 → 全刷。
     * 阈值默认运行期按带面积 25%（Phase 5 随带尺寸参数化；
     * 编译期 -D 覆盖正值时以覆盖值为准） */
    int max_diff = (STANDBY_PARTIAL_MAX_DIFF_PX >= 0)
                 ? STANDBY_PARTIAL_MAX_DIFF_PX
                 : SB_QUOTE_W * SB_QUOTE_H / 4;
    const char *mode;
    if (diff_px > max_diff) {
        mode = "full (large change)";
    } else if (refresh_gfx_before_partial_n(STANDBY_PARTIAL_MAX_N)) {
        mode = "full (partial count threshold)";
    } else {
        mode = "partial direct (single-phase diff)";
    }

    if (mode[0] == 'p') {
        /* 单段直接差分：画布已是完整新帧（预检时绘好：旧字位白 +
         * 新字位黑），一次局刷同 pass 驱动两方向翻转像素 */
        epd_gfx_flush_window_passes(SB_QUOTE_X0, SB_QUOTE_Y0,
                                    SB_QUOTE_W, SB_QUOTE_H, 1);
        memcpy(s_quote_shadow, s_quote_scratch, s_quote_bytes);
    } else {
        /* 全刷：整页重绘（含出处），影子随 render_full 内部同步 */
        standby_render_full();
    }

    s_last_quote = quote;
    sb_hold_win_sync();    /* 三色屏：冻结自然窗（自动轮换停用） */
    LOG_I("quote rotate: idx=%d, %s, diff=%dpx, partials=%u", quote, mode,
          diff_px, refresh_partial_count());
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

    /* 配网页 / LAN 接收页 / 快捷菜单接管屏幕期间不绘制 */
    if (wifi_config_ui_is_active() || lan_server_is_active() ||
        menu_ui_is_active()) return;

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

    /* ---- 引文轮换：5 分钟窗下标变化 → 智能分流刷新 ----
     * 首绘/校时跳变（-2 无基准）走全刷；其余先白后画 + 差分分流
     * （常规轮换走局刷引文带，大面积变化/局刷超阈值自动全刷） */
    int quote = sb_quote_now();
    if (quote == s_last_quote) return;

    if (s_last_quote == -2 || !s_shadow_valid)
        standby_render_full();   /* 无屏幕基准：整页全刷 */
    else
        standby_render_quote();  /* 有基准：局刷优先智能分流 */
}
