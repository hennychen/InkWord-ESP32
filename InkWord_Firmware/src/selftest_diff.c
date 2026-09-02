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
