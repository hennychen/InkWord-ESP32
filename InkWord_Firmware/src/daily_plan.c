/**
 * @file daily_plan.c
 * @brief 每日学习计划实现（v1.2 T2.4 起，见 daily_plan.h 模块边界）
 */
#include "daily_plan.h"
#include "learning_state.h"
#include "deck_manager.h"

#include "nvs.h"
#include "settings_keys.h"   /* P2b：NVS 键权威表 */
#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* 自治钟公共导出（standby_page.c；考试倒计时基准。native-test 由
 * 测试文件提供桩实现，learning_state.c 同款先例） */
extern int64_t standby_time_now(void);

#define DP_DEFAULT_GOAL 20    /* 百词斩同款默认（ROADMAP v1.2 行 2） */
#define DP_MIN_GOAL     5
#define DP_MAX_GOAL     100
#define DP_STEP_GOAL    5

#define DP_EXAM_MAX_DAYS 99   /* 倒计时上限（u8 设置档位一致） */

/* ---- 卡组日期数学（Howard Hinnant civil 算法，learning_state.c 同源副本；
 *      native 可测的独立小函数，模块零状态纪律保持） ---- */

static int64_t civil_days(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;                                  /* [0,399] */
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; /* [0,365] */
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          /* [0,146096] */
    return era * 146097 + doe - 719468;
}

static int32_t civil_from_days(int64_t z)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    return (int32_t)(y * 10000 + m * 100 + d);
}

/* epoch -> UTC+8 日历 yyyymmdd（未同步/区间外返回 0） */
static int32_t epoch_ymd(int64_t epoch)
{
    time_t t = (time_t)(epoch + 8 * 3600);
    struct tm tmv;
    if (!gmtime_r(&t, &tmv)) return 0;
    return (int32_t)((tmv.tm_year + 1900) * 10000 +
                     (tmv.tm_mon + 1) * 100 + tmv.tm_mday);
}

/* ---- 目标量：NVS u8 键按卡组分派（T5.5 兑现 v1.2 预留注释；
 *      默认组沿用 set_daily，其余 sd_<id>（3+7=10 ≤ 键名 15 上限） ---- */

static void dp_goal_key(char *out, size_t cap, const char *deck_id)
{
    if (!deck_id || !deck_id[0]) snprintf(out, cap, NVS_KEY_SET_DAILY);
    else                         snprintf(out, cap, NVS_KEY_SD_PFX "%s", deck_id);
}

int daily_plan_goal_deck(const char *deck_id)
{
    /* 低频显示路径（概况页/词书页角标）直读 NVS，learning_state 键同款
     * 风格；值域校验：库外值（老键类型变化等）回落默认 */
    char key[12];
    dp_goal_key(key, sizeof(key), deck_id);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = DP_DEFAULT_GOAL;
        bool hit = nvs_get_u8(h, key, &v) == ESP_OK;   /* 无键保持默认 */
        nvs_close(h);
        if (hit && v >= DP_MIN_GOAL && v <= DP_MAX_GOAL && v % DP_STEP_GOAL == 0)
            return v;
    }
    return DP_DEFAULT_GOAL;
}

int daily_plan_goal(void)
{
    return daily_plan_goal_deck(deck_manager_active_id());
}

void daily_plan_set_goal(int n)
{
    if (n < DP_MIN_GOAL) n = DP_MIN_GOAL;
    if (n > DP_MAX_GOAL) n = DP_MAX_GOAL;
    n = n / DP_STEP_GOAL * DP_STEP_GOAL;  /* 步进归整（防御） */

    char key[12];
    dp_goal_key(key, sizeof(key), deck_manager_active_id());

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, (uint8_t)n);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool daily_plan_done(void)
{
    /* T5.5 按组口径：分子读 SD01 表（全局 lr_stats.today_new 跨组累计
     * 是 T4.2 特性，配额判据混用会虚达标）；due 仍为当前词池口径 */
    return learning_state_deck_today_new(deck_manager_active_id()) >=
               daily_plan_goal() &&
           learning_state_due_count() == 0;
}

/* ---- 考试倒计时（T5.5）：u32 set_exam = 目标日 yyyymmdd（0=未设） ---- */

static int32_t exam_ymd_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint32_t v = 0;
        bool hit = nvs_get_u32(h, NVS_KEY_SET_EXAM, &v) == ESP_OK;
        nvs_close(h);
        if (hit && v >= 20250101 && v <= 20991231) return (int32_t)v;
    }
    return 0;
}

int exam_days_left(void)
{
    int32_t exam = exam_ymd_load();
    if (exam == 0) return 0;
    int64_t ep = standby_time_now();
    if (ep <= 0) return 0;                        /* 未同步：无基准 */
    int32_t today = epoch_ymd(ep);
    if (today == 0) return 0;

    int64_t left = civil_days(exam / 10000, exam % 10000 / 100, exam % 100) -
                   civil_days(today / 10000, today % 10000 / 100, today % 100);
    if (left < 1 || left > DP_EXAM_MAX_DAYS) return 0;   /* 已过/超上限归 0 */
    return (int)left;
}

void exam_set_days(int n)
{
    if (n > DP_EXAM_MAX_DAYS) n = DP_EXAM_MAX_DAYS;

    int64_t ep = standby_time_now();
    int32_t today = ep > 0 ? epoch_ymd(ep) : 0;
    if (n <= 0 || today == 0) {
        /* 清除（n=0），或时钟未同步拒绝设置（保持未设，设置页回显"关"） */
        if (n <= 0) {
            nvs_handle_t h;
            if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_u32(h, NVS_KEY_SET_EXAM, 0);
                nvs_commit(h);
                nvs_close(h);
            }
        }
        return;
    }

    int32_t ymd = civil_from_days(civil_days(today / 10000, today % 10000 / 100,
                                             today % 100) + n);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, NVS_KEY_SET_EXAM, (uint32_t)ymd);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool exam_urgent(void)
{
    int d = exam_days_left();
    return d >= 1 && d <= 7;
}
