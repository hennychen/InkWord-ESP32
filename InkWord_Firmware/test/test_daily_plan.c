/**
 * @file test_daily_plan.c
 * @brief daily_plan 按组配额 + 考试倒计时测试（v1.5 T5.5，2026-08-25）
 *
 * daily_plan.c 经 build_src_filter 链入 native 单 program；其外部依赖
 * （nvs / 自治钟 / deck_manager / learning_state 分子口径）在本文件
 * 提供桩实现：nvs 内存 kv 表、可拨 g_now 时钟、可切 g_active 组。
 * 日期数学用 timegm 构造已知 epoch（daily_plan 内部 +8h 取 UTC+8 日历）。
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unity.h>

#include "daily_plan.h"
#include "nvs.h"              /* stubs/nvs.h 优先命中（mock 实现） */

/* ---- nvs 内存 mock（stubs/nvs.h 声明的单实例实现） ---- */

typedef struct {
    char     key[16];
    uint32_t val;
} nv_kv_t;

static nv_kv_t s_kv[16];

void nvs_mock_reset(void)
{
    memset(s_kv, 0, sizeof(s_kv));
}

static nv_kv_t *kv_find(const char *key)
{
    for (int i = 0; i < (int)(sizeof(s_kv) / sizeof(s_kv[0])); i++)
        if (strcmp(s_kv[i].key, key) == 0) return &s_kv[i];
    return NULL;
}

static nv_kv_t *kv_slot(const char *key)
{
    nv_kv_t *e = kv_find(key);
    if (e) return e;
    for (int i = 0; i < (int)(sizeof(s_kv) / sizeof(s_kv[0])); i++) {
        if (s_kv[i].key[0] == '\0') {
            snprintf(s_kv[i].key, sizeof(s_kv[i].key), "%s", key);
            return &s_kv[i];
        }
    }
    return NULL;   /* 表满（16 键远超用例需求，防御） */
}

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *out)
{
    (void)ns; (void)mode;
    *out = 1;
    return ESP_OK;
}

void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }

esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out)
{
    (void)h;
    nv_kv_t *e = kv_find(key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    *out = (uint8_t)e->val;
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v)
{
    (void)h;
    nv_kv_t *e = kv_slot(key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    e->val = v;
    return ESP_OK;
}

esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out)
{
    (void)h;
    nv_kv_t *e = kv_find(key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    *out = e->val;
    return ESP_OK;
}

esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t v)
{
    (void)h;
    nv_kv_t *e = kv_slot(key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    e->val = v;
    return ESP_OK;
}

/* ---- 被测源外部符号桩（daily_plan.c 引用集） ---- */

static int64_t     g_now;      /* 自治钟 epoch；<=0 模拟未同步 */
static const char *g_active;   /* 活跃卡组 id（""=默认） */
static int         g_deck_new; /* learning_state_deck_today_new 桩值 */
static int         g_due;      /* learning_state_due_count 桩值 */

int64_t standby_time_now(void) { return g_now; }

const char *deck_manager_active_id(void) { return g_active; }

int learning_state_deck_today_new(const char *deck_id)
{
    (void)deck_id;
    return g_deck_new;
}

int learning_state_due_count(void) { return g_due; }

/* UTC 时刻 -> epoch（daily_plan 内部 +8h 取北京日历，故 4:00Z=正午） */
static int64_t utc_epoch(int y, int mo, int d, int h)
{
    struct tm t = {0};
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h;
    return (int64_t)timegm(&t);
}

static void dp_reset(void)
{
    nvs_mock_reset();
    g_now      = utc_epoch(2026, 8, 25, 4);   /* 北京 2026-08-25 12:00 */
    g_active   = "";
    g_deck_new = 0;
    g_due      = 0;
}

/* ---- 目标量：键按卡组分派 ---- */

void test_dp_goal_default_when_no_key(void)
{
    dp_reset();
    TEST_ASSERT_EQUAL_INT(20, daily_plan_goal());
    TEST_ASSERT_EQUAL_INT(20, daily_plan_goal_deck(""));
    TEST_ASSERT_EQUAL_INT(20, daily_plan_goal_deck("poem"));
}

void test_dp_goal_deck_key_dispatch(void)
{
    dp_reset();
    nvs_handle_t h;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open("inkword", NVS_READWRITE, &h));
    nvs_set_u8(h, "sd_poem", 10);
    nvs_close(h);

    /* 各组互不串：poem=10，默认组无键回落 20 */
    TEST_ASSERT_EQUAL_INT(10, daily_plan_goal_deck("poem"));
    TEST_ASSERT_EQUAL_INT(20, daily_plan_goal_deck(""));
    TEST_ASSERT_EQUAL_INT(20, daily_plan_goal_deck("qa"));

    /* 活跃组薄壳路由 */
    g_active = "poem";
    TEST_ASSERT_EQUAL_INT(10, daily_plan_goal());
}

void test_dp_goal_invalid_value_falls_back(void)
{
    dp_reset();
    nvs_handle_t h;
    nvs_open("inkword", NVS_READWRITE, &h);
    nvs_set_u8(h, "sd_bad", 7);      /* 库外（非 5 步进） */
    nvs_set_u8(h, "set_daily", 120); /* 超上限 */
    nvs_close(h);
    TEST_ASSERT_EQUAL_INT(20, daily_plan_goal_deck("bad"));
    TEST_ASSERT_EQUAL_INT(20, daily_plan_goal());
}

void test_dp_set_goal_clamps_and_steps(void)
{
    dp_reset();
    daily_plan_set_goal(3);      /* 下钳 5 */
    TEST_ASSERT_EQUAL_INT(5, daily_plan_goal());
    daily_plan_set_goal(102);    /* 上钳 100 */
    TEST_ASSERT_EQUAL_INT(100, daily_plan_goal());
    daily_plan_set_goal(23);     /* 步进归整 20 */
    TEST_ASSERT_EQUAL_INT(20, daily_plan_goal());
}

void test_dp_set_goal_writes_active_deck_key(void)
{
    dp_reset();
    nvs_handle_t h;
    nvs_open("inkword", NVS_READWRITE, &h);
    nvs_set_u8(h, "set_daily", 15);   /* 默认组既有值 */
    nvs_close(h);

    g_active = "poem";
    daily_plan_set_goal(30);

    uint8_t v = 0;
    nvs_open("inkword", NVS_READONLY, &h);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_u8(h, "sd_poem", &v));
    TEST_ASSERT_EQUAL_UINT(30, v);
    /* 默认组键不被串写（组隔离） */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_u8(h, "set_daily", &v));
    TEST_ASSERT_EQUAL_UINT(15, v);
    nvs_close(h);
}

void test_dp_done_uses_deck_numerator(void)
{
    dp_reset();
    daily_plan_set_goal(10);

    /* 按组分子 8 < 10：未达标（全局口径无关） */
    g_deck_new = 8;
    g_due = 0;
    TEST_ASSERT_FALSE(daily_plan_done());

    g_deck_new = 10;                 /* 达标且无到期 */
    TEST_ASSERT_TRUE(daily_plan_done());

    g_due = 3;                       /* 达标但有到期词 */
    TEST_ASSERT_FALSE(daily_plan_done());
}

/* ---- 考试倒计时 ---- */

void test_exam_unset_returns_zero(void)
{
    dp_reset();
    TEST_ASSERT_EQUAL_INT(0, exam_days_left());
    TEST_ASSERT_FALSE(exam_urgent());
}

void test_exam_set_and_countdown(void)
{
    dp_reset();
    exam_set_days(7);
    TEST_ASSERT_EQUAL_INT(7, exam_days_left());
    TEST_ASSERT_TRUE(exam_urgent());    /* ≤7 冲刺窗口 */
}

void test_exam_urgent_boundary(void)
{
    dp_reset();
    exam_set_days(8);
    TEST_ASSERT_EQUAL_INT(8, exam_days_left());
    TEST_ASSERT_FALSE(exam_urgent());   /* 8 天外不打乱 FSRS 节奏 */
}

void test_exam_expires_to_zero(void)
{
    dp_reset();
    exam_set_days(1);
    TEST_ASSERT_EQUAL_INT(1, exam_days_left());

    g_now = utc_epoch(2026, 8, 27, 4);  /* 两天后（北京 8/27） */
    TEST_ASSERT_EQUAL_INT(0, exam_days_left());
    TEST_ASSERT_FALSE(exam_urgent());
}

void test_exam_clear(void)
{
    dp_reset();
    exam_set_days(7);
    exam_set_days(0);
    TEST_ASSERT_EQUAL_INT(0, exam_days_left());
}

void test_exam_refused_when_clock_unsynced(void)
{
    dp_reset();
    g_now = -1;                      /* 自治钟未同步 */
    exam_set_days(5);
    TEST_ASSERT_EQUAL_INT(0, exam_days_left());   /* 拒写：保持未设 */
}

void test_exam_month_rollover_math(void)
{
    dp_reset();
    g_now = utc_epoch(2026, 1, 30, 16);  /* 北京 2026-01-31 正午 */
    exam_set_days(1);
    TEST_ASSERT_EQUAL_INT(1, exam_days_left());   /* 目标 2/1（跨月） */

    g_now = utc_epoch(2026, 2, 1, 16);   /* 北京 2/2：已过 */
    TEST_ASSERT_EQUAL_INT(0, exam_days_left());
}

void test_exam_leap_year_math(void)
{
    dp_reset();
    g_now = utc_epoch(2028, 2, 28, 4);   /* 北京 2028-02-28（闰年） */
    exam_set_days(1);
    TEST_ASSERT_EQUAL_INT(1, exam_days_left());   /* 目标 2/29 存在 */

    g_now = utc_epoch(2028, 2, 29, 4);   /* 北京 2/29 当天：仍剩 0（当日） */
    TEST_ASSERT_EQUAL_INT(0, exam_days_left());
}
