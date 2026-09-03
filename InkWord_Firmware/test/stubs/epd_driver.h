/**
 * @file epd_driver.h
 * @brief epd_gfx 尺寸桩（native-test 专用，-I test/stubs 优先命中）
 *
 * 仅覆盖 layout_profile.c 引用的 width/height 接口（其余 epd_gfx_*
 * 绘图 API 属 L3 硬件绑定，被测源不用）。stub_gfx_w/h 单实例：声明在
 * 头，实现由 test_layout_profile.c 提供（native 单 program 链接，
 * 避免头文件 static 多实例状态分裂；同 stubs/nvs.h 模式）。
 */
#ifndef STUB_EPD_DRIVER_H
#define STUB_EPD_DRIVER_H

#ifdef __cplusplus
extern "C" {
#endif

/** 测试可设的 gfx UI 尺寸（epd_driver_init 后运行期恒定的替身） */
extern int stub_gfx_w, stub_gfx_h;

int epd_gfx_width(void);
int epd_gfx_height(void);

#ifdef __cplusplus
}
#endif
#endif /* STUB_EPD_DRIVER_H */
