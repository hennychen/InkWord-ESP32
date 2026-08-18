/**
 * @file refresh_scheduler.c
 * @brief 刷新调度器实现 (Task F-13)
 *
 * 策略：每次局刷计数 +1，达到阈值时先全屏清白再按数据全刷，
 * 计数归零。全屏刷新不计数。
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
        epd_clear_screen();
        s_partial_cnt = 0;
        return true; /* 已清屏：调用方需整屏重绘后全刷 */
    }
    s_partial_cnt++;
    return false;
}

bool refresh_gfx_before_partial(void)
{
    return refresh_gfx_before_partial_n(s_threshold);
}

void refresh_submit(Rect area, const uint8_t *data)
{
    /* 达到阈值：先清残影再继续 */
    if (s_partial_cnt >= s_threshold) {
        LOG_I("partial count reached %u, auto full refresh", s_partial_cnt);
        epd_clear_screen();
        s_partial_cnt = 0;
    }

    if (area.x == 0 && area.y == 0 &&
        area.w == EPD_WIDTH && area.h == EPD_HEIGHT) {
        /* 全屏请求 -> 全刷，不计数 */
        epd_full_refresh(data);
    } else {
        /* 局部请求 -> 局刷并计数 */
        epd_partial_refresh(area.x, area.y, area.w, area.h, data);
        s_partial_cnt++;
        LOG_D("partial #%u on [%d,%d,%d,%d]", s_partial_cnt, area.x, area.y, area.w, area.h);
    }
}
