/**
 * @file refresh_scheduler.c
 * @brief 刷新调度器实现 (Task F-13)
 *
 * 策略（2026-08-20 无窗口方案定稿）：每次局刷计数 +1，达到阈值返回
 * true 由调用方整屏重绘后走真全刷（调度器不预清屏 —— 双 RAM 差分
 * 局刷自身无残影，全刷为低频深度保养），计数归零。
 */
#include "refresh_scheduler.h"
#include "epd_driver.h"
#include "debug_log.h"

static const char *TAG = "REFRESH";
static uint8_t s_threshold = 8;
static uint8_t s_partial_cnt = 0;

void refresh_scheduler_init(uint8_t partial_threshold)
{
    s_threshold = (partial_threshold > 0) ? partial_threshold : 8;
    s_partial_cnt = 0;
    LOG_I("refresh scheduler init, partial threshold=%u", s_threshold);
}

void refresh_force_full(void)
{
    LOG_I("forced full refresh (clear ghosting), prior partials=%u", s_partial_cnt);
    epd_clear_screen();
    s_partial_cnt = 0;
}

void refresh_notify_full_done(void)
{
    /* 外部路径（LAN 直传）已做全刷，等价于残影清理，计数归零即可 */
    s_partial_cnt = 0;
}

uint8_t refresh_partial_count(void)
{
    return s_partial_cnt;
}

bool refresh_gfx_before_partial_n(uint8_t threshold)
{
    if (s_partial_cnt >= threshold) {
        LOG_I("partial count reached %u (threshold %u), auto full refresh",
              s_partial_cnt, threshold);
        /* 2026-08-18：不再预清屏（黑白深清 2x1.8s 太慢且无必要 ——
         * 真全刷（无窗口直写）已能洗净残影；调用方整屏重绘走真全刷即可 */
        s_partial_cnt = 0;
        return true; /* 调用方需整屏重绘后全刷 */
    }
    s_partial_cnt++;
    return false;
}

bool refresh_gfx_before_partial(void)
{
    return refresh_gfx_before_partial_n(s_threshold);
}
