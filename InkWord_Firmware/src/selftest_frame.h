/**
 * @file selftest_frame.h
 * @brief T2.2 黄金帧回归自检（INKWORD_GOLDEN_FRAME 门控，demo env 专用）
 *
 * 开机顺序渲染样板页 → 画布整帧回读（epd_gfx_read_window）→ 与
 * selftest_golden.h 内嵌基准逐字节 diff；无基准页输出 base64 dump
 * 供 tools/gen_golden.py 生成基线。生产 env 由 selftest_frame.c 整
 * 文件门控空展开（零体积/零符号）。
 */
#ifndef INKWORD_SELFTEST_FRAME_H
#define INKWORD_SELFTEST_FRAME_H

#ifdef __cplusplus
extern "C" {
#endif

/** 跑全页自检序列并挂起（demo 单用途固件不进 loop；见 .c 页面集） */
void selftest_frame_run(void);

#ifdef __cplusplus
}
#endif

#endif /* INKWORD_SELFTEST_FRAME_H */
