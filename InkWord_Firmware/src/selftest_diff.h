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

#endif /* INKWORD_SELFTEST_DIFF_H */
