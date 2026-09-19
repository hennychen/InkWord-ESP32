/**
 * @file epd_geom.c
 * @brief 显示链路几何纯函数层（T2.1，修 D1）
 *
 * 公式自 epd_driver.cpp transpose_to_plane / gfx_rect_to_panel /
 * bw_layer_color / ac_layer_color 原样提取（行为等价重构，编译期
 * 消化）；本文件不得持有状态或 include Arduino 头——native-test
 * 直接链入。
 */
#include "epd_geom.h"

/* GFXcanvas1 1bpp 画布色（数值与 GxEPD2 的 GxEPD_BLACK/GxEPD_WHITE
 * 一致：0x0000 → bit=0 黑 / 0xFFFF → bit=1 白，画布 bit=1 白） */
static const uint16_t GEOM_CANVAS_BLACK = 0x0000;
static const uint16_t GEOM_CANVAS_WHITE = 0xFFFF;

void epd_geom_transpose(const uint8_t *src, int gw, int gh,
                        uint8_t *plane, int pstride, int rot)
{
    const int stride = (gw + 7) / 8;          /* 画布行宽字节 */
    for (int cy = 0; cy < gh; cy++) {
        const uint8_t *row = src + cy * stride;
        for (int cx = 0; cx < gw; cx++) {
            if (row[cx >> 3] & (0x80 >> (cx & 7))) {
                int px, py;
                if (rot == 0)      { px = cx;          py = cy; }
                else if (rot == 1) { px = gh - 1 - cy; py = cx; }        /* 现役 */
                else if (rot == 2) { px = gw - 1 - cx; py = gh - 1 - cy; }
                else               { px = cy;          py = gw - 1 - cx; }
                plane[py * pstride + (px >> 3)] |= (uint8_t)(0x80 >> (px & 7));
            }
        }
    }
}

void epd_geom_rect_to_panel(int x, int y, int w, int h,
                            int gw, int gh, int rot,
                            uint16_t *px, uint16_t *py,
                            uint16_t *pw, uint16_t *ph)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > gw) w = gw - x;
    if (y + h > gh) h = gh - y;
    if (w <= 0 || h <= 0) { *pw = 0; *ph = 0; return; }

    uint16_t rx, ry, rw, rh;
    switch (rot) {
    case 0:  rx = (uint16_t)x;              ry = (uint16_t)y;              rw = (uint16_t)w; rh = (uint16_t)h; break;
    case 1:  rx = (uint16_t)(gh - y - h);   ry = (uint16_t)x;              rw = (uint16_t)h; rh = (uint16_t)w; break; /* 现役 */
    case 2:  rx = (uint16_t)(gw - x - w);   ry = (uint16_t)(gh - y - h);   rw = (uint16_t)w; rh = (uint16_t)h; break;
    default: rx = (uint16_t)y;              ry = (uint16_t)(gw - x - w);   rw = (uint16_t)h; rh = (uint16_t)w; break;
    }

    *px = rx; *py = ry; *pw = rw; *ph = rh;
}

uint16_t epd_geom_bw_layer_color(uint16_t color)
{
    return (color == EPD_GFX_WHITE) ? GEOM_CANVAS_WHITE : GEOM_CANVAS_BLACK;
}

uint16_t epd_geom_ac_layer_color(uint16_t color)
{
    return (color == EPD_GFX_ACCENT) ? GEOM_CANVAS_WHITE : GEOM_CANVAS_BLACK;
}
