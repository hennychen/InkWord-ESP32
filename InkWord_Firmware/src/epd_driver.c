/**
 * @file epd_driver.c
 * @brief 墨水屏驱动封装实现 (Task F-05 ~ F-09)，基于 EPDiy V7 API。
 */
#include "epd_driver.h"
#include "debug_log.h"
#include "epdiy.h"

static const char *TAG = "EPD";

/* 全屏帧缓冲（1 字节 8 像素，1=黑） */
#define FB_SIZE  (EPD_WIDTH / 8 * EPD_HEIGHT)
static uint8_t s_framebuffer[FB_SIZE];

/* 标记是否已初始化，避免重复 epd_init */
static bool s_inited = false;

int epd_driver_init(void)
{
    if (s_inited) {
        LOG_W("epd_driver already initialized, skip");
        return 0;
    }

    /* EPDiy V7: 初始化硬件 + 选择显示型号 */
    epd_init(&epd_board_v7_v3, &ED097TC2, EPD_OPTIONS_DEFAULT);
    LOG_I("EPDiy initialized (board=v7_v3, display=ED097TC2)");

    /* 读一次厂商 ID 验证连接 */
    char mfr[32] = {0};
    uint16_t id = epd_get_manufacturer(mfr, sizeof(mfr));
    if (id == 0xFFFF) {
        LOG_E("EPD manufacturer ID readback failed (0xFFFF), check wiring");
        return -1;
    }
    LOG_I("EPD manufacturer='%s' id=0x%04X", mfr, id);

    s_inited = true;
    return 0;
}

void epd_power_on(void)
{
    epd_poweron();
}

void epd_power_off(void)
{
    epd_poweroff();
}

uint16_t epd_get_manufacturer(char *manufacturer, size_t len)
{
    /* EPDiy 提供底层读取：epd_read_display_id() 返回厂商寄存器值。
     * 高 8 位通常为厂商代号（Pervasive=0x03, Boe=0x02 等）。 */
    uint16_t id = (uint16_t)epd_read_display_id();
    if (manufacturer && len > 0) {
        const char *name = "unknown";
        switch ((id >> 8) & 0xFF) {
            case 0x00: name = "Pervasive"; break;
            case 0x02: name = "BOE";       break;
            case 0x03: name = "Primax";    break;
            default: break;
        }
        snprintf(manufacturer, len, "%s", name);
    }
    return id;
}

void epd_full_refresh(const uint8_t *framebuffer)
{
    if (!s_inited) {
        LOG_E("epd not initialized before full refresh");
        return;
    }

    epd_poweron();
    EpdRect full = { .x = 0, .y = 0, .width = EPD_WIDTH, .height = EPD_HEIGHT };

    if (framebuffer) {
        /* MODE_GL16：16 级灰度全刷，兼顾质量与残影清除 */
        epd_draw_image(full, (uint8_t *)framebuffer, MODE_GL16);
    } else {
        /* 无数据则清白 */
        epd_clear_area(full);
    }

    /* 全刷后下电节能 */
    epd_poweroff();
    LOG_D("full refresh done");
}

void epd_clear_screen(void)
{
    if (!s_inited) return;
    epd_poweron();
    EpdRect full = { .x = 0, .y = 0, .width = EPD_WIDTH, .height = EPD_HEIGHT };
    epd_clear_area(full);
    epd_poweroff();
}

void epd_partial_refresh(int x, int y, int w, int h, const uint8_t *data)
{
    if (!s_inited) {
        LOG_E("epd not initialized before partial refresh");
        return;
    }

    epd_poweron();
    EpdRect area = { .x = x, .y = y, .width = w, .height = h };

    /* MODE_A2：快速局刷，1bit 数据，适合黑白文字翻页。
     * 注意：A2 模式不主动清白背景，需调用方自行准备干净缓冲。 */
    epd_draw_image(area, (uint8_t *)data, MODE_A2);
    /* 局刷不下电，保证连续翻页时序稳定；由刷新调度器在空闲时统一 poweroff */
    LOG_D("partial refresh area=[%d,%d,%d,%d]", x, y, w, h);
}

void epd_deep_sleep(void)
{
    if (!s_inited) {
        LOG_W("epd not initialized, nothing to sleep");
        return;
    }
    /* 先下高压电路，再触发面板深度休眠（DSLP）。
     * EPDiy 的 epd_poweroff 已将面板置于 low-power；这里额外保证断电。 */
    epd_poweroff();
    s_inited = false;
    LOG_I("EPD entered deep sleep");
}

uint8_t *epd_get_framebuffer(void)
{
    return s_framebuffer;
}
