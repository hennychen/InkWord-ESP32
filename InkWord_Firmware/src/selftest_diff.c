/**
 * @file selftest_diff.c
 * @brief 黄金帧逐字节 diff 实现（T2.2，纯 C 无硬件依赖）
 */
#include "selftest_diff.h"

int selftest_diff_bytes(const uint8_t *a, const uint8_t *b, size_t len,
                        long *first_diff_off)
{
    int n = 0;
    long first = -1;
    for (size_t i = 0; i < len; i++) {
        if (a[i] != b[i]) {
            n++;
            if (first < 0) first = (long)i;
        }
    }
    if (first_diff_off) *first_diff_off = first;
    return n;
}

void selftest_diff_mask_white(uint8_t *buf, int w, int h,
                               const int (*rects)[4], int n)
{
    int stride = (w + 7) / 8;
    for (int r = 0; r < n; r++) {
        int x0 = rects[r][0], y0 = rects[r][1];
        int rw = rects[r][2], rh = rects[r][3];
        if (x0 < 0) { rw += x0; x0 = 0; }
        if (y0 < 0) { rh += y0; y0 = 0; }
        if (x0 + rw > w) rw = w - x0;
        if (y0 + rh > h) rh = h - y0;
        if (rw <= 0 || rh <= 0) continue;
        for (int y = y0; y < y0 + rh; y++) {
            uint8_t *row = buf + (size_t)y * stride;
            for (int x = x0; x < x0 + rw; x++)
                row[x >> 3] &= (uint8_t)~(0x80 >> (x & 7));  /* 置白=清位 */
        }
    }
}
