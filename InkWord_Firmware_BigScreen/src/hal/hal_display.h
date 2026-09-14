#pragma once
// ============================================================
// hal_display.h — 显示抽象接口（方案 §4.1，Phase 2 实现）
//
// 架构红线：业务逻辑只允许经本接口访问显示，禁止直接调用 epdiy
// API（实现文件 hal/hal_display_epdiy.c 待 Phase 2 落地）。
// Phase 1 bring-up 期间 test_basic.c 直连 epdiy 属于诊断代码豁免。
// ============================================================
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int width;
    int height;
    int bpp;  // 4 for epdiy
} hal_display_info_t;

// 刷新模式（对 epdiy MODE_* 的子集映射，Phase 2 可扩展）
enum hal_draw_mode {
    HAL_DRAW_MODE_DU,    // 差分快速（局部区域）
    HAL_DRAW_MODE_GL16,  // 灰度保真
    HAL_DRAW_MODE_GC16,  // 高质量全刷
};

// 初始化（内部处理 epdiy init + VCOM + 波形）
esp_err_t hal_display_init(void);

// 获取显示信息
esp_err_t hal_display_get_info(hal_display_info_t* info);

// 获取帧缓冲指针（epdiy 内部管理，4bpp）
uint8_t* hal_display_get_framebuffer(void);

// 刷新全屏（封装 epd_hl_update_screen）
esp_err_t hal_display_update_full(enum hal_draw_mode mode);

// 局部刷新
esp_err_t hal_display_update_area(int x, int y, int w, int h);

// 清屏
esp_err_t hal_display_clear(void);

// 关机（安全断电序列）
void hal_display_poweroff(void);

#ifdef __cplusplus
}
#endif
