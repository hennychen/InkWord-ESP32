/**
 * @file epd_geom.h
 * @brief 显示链路几何纯函数层（T2.1，修 D1）
 *
 * 转置四方向 / 窗口映射 / 调色板降级三组公式的唯一权威——自
 * epd_driver.cpp 提取为无状态纯 C（缓冲+宽高参数化剥离 GFXcanvas1
 * 依赖），获得 host 可跑的真值表测试（native-test）；rot=0/2/3 从
 * 「仅保证编译」变为有自动化验证。
 *
 * 语义锚点（PANEL_COMPAT_DESIGN §6.3 四方向映射表，对齐 GxEPD2_BW
 * setRotation，_reverse=false，现役 rot=1 顺时针 90°）。
 */
#ifndef INKWORD_EPD_GEOM_H
#define INKWORD_EPD_GEOM_H

#include <stdint.h>
#include "epd_gfx.h"     /* EPD_GFX_* 逻辑色值（大屏迁移：gfx 层单真相源） */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 画布层 → 面板平面四方向转置
 *
 * src：行主序 MSB-first 1bpp（bit=1 置位：B/W 层白 / AC 层红），
 *      gw x gh 像素，行宽 (gw+7)/8 字节；
 * plane：出参缓冲（pstride 字节/行，高按 rot 取 gw 或 gh），置位用
 *      |= 合并——调用方负责预清零（差分帧语义）；
 * rot：0=直通 / 1=顺 90°（现役）/ 2=180° / 3=逆 90°。
 */
void epd_geom_transpose(const uint8_t *src, int gw, int gh,
                        uint8_t *plane, int pstride, int rot);

/**
 * GFX 窗口 → 面板窗口（四方向 + 越界钳位）
 *
 * 入参 gfx 侧窗口 (x,y,w,h) 与画布尺寸 (gw,gh)；负偏移收缩、
 * 超界裁边，完全出界时 *pw=*ph=0（调用方以 0 尺寸跳过刷新）。
 */
void epd_geom_rect_to_panel(int x, int y, int w, int h,
                            int gw, int gh, int rot,
                            uint16_t *px, uint16_t *py,
                            uint16_t *pw, uint16_t *ph);

/**
 * 逻辑色 → B/W 层画布色（§9.4 真值表）
 *
 * WHITE→0xFFFF（位全 1 白），BLACK/ACCENT/AUX→0x0000（红像素
 * 需 B/W 位为黑，IL0398 真值表 (0,1)=红）；BW 单平面面板仅本
 * 层参与——ACCENT/AUX 退化为 BLACK 即 BW 降级规则本体。
 */
uint16_t epd_geom_bw_layer_color(uint16_t color);

/**
 * 逻辑色 → AC 层画布色（§9.4 真值表）
 *
 * ACCENT→0xFFFF（位全 1＝红，非「白」——命名易误导，2026-08-22
 * 真机勘误留档），其余→0x0000（BLACK 绘制必须清红防旧红残留）。
 */
uint16_t epd_geom_ac_layer_color(uint16_t color);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_EPD_GEOM_H */
