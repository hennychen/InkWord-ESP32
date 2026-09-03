/**
 * @file selftest_diff.h
 * @brief 黄金帧逐字节 diff（T2.2 修 D2，纯函数）
 *
 * 独立成模块的原因：selftest_frame.c 整体 INKWORD_GOLDEN_FRAME 门控
 * （生产 env 空展开），而 diff 逻辑需 native-test 覆盖灵敏度
 * （1 字节差异检出 / 首异偏移正确性）——零硬件依赖单编即可挂入
 * native-test 的 build_src_filter。
 */
#ifndef INKWORD_SELFTEST_DIFF_H
#define INKWORD_SELFTEST_DIFF_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief 逐字节比较两帧
 * @param a/b 帧缓冲（行主序 MSB-first，格式见 epd_gfx_read_window）
 * @param len 字节数
 * @param first_diff_off 非空时写入首个差异字节偏移（无差异写 -1）
 * @return 差异字节数（0 = 逐位一致，灵敏度 1 字节）
 */
int selftest_diff_bytes(const uint8_t *a, const uint8_t *b, size_t len,
                        long *first_diff_off);

/**
 * @brief 动态区域 mask：帧 buf 内 rects[n] 矩形就位置白（T3.2）
 * @param buf 帧缓冲（行主序 MSB-first，bit=1=黑，同 read_window）
 * @param w/h 帧像素宽高（stride=(w+7)/8 派生）
 * @param rects 矩形表 {x, y, w, h}（越界自动钳位，负尺寸跳过）
 * @param n 矩形数
 *
 * dump 前双侧同构施加→基线生成天然含 mask，diff 双侧一致，
 * 无需改比较逻辑（menu 徽标列等动态区域差异不计数）。 */
void selftest_diff_mask_white(uint8_t *buf, int w, int h,
                               const int (*rects)[4], int n);

#endif /* INKWORD_SELFTEST_DIFF_H */
