/**
 * @file epd_driver.h
 * @brief 墨水屏驱动封装 — 多面板 GFX/C API 薄适配层（L3）
 *
 * 硬件：ESP32-S3 + 转接板（板级轴 INKWORD_BOARD_*，默认 EVK011，
 *       可切 v1.4 通用板，见 gpio_config.h）+ 面板轴（构建矩阵
 *       INKWORD_PANEL_*，默认 DEPG0370 3.7" BW / 可选 E042A13
 *       4.2" BWR 三色，见 epd_panel.h）
 * 接口：4 线 SPI（BS1=LOW）；驱动核心为 GxEPD2，面板序列封装于
 *       panels/（L0）经 L2 epd_panel.h desc.ops 分发
 * 升压：两板均自主驱动（EVK011 由 COG 经 GDR 驱动板上分立 boost /
 *       v1.4 板载自主），MCU 唯一电源职责是供 VCI 3.3V，无 GDR/RESE 信号
 *
 * 几何/帧长（Phase 2 起全部运行期取自 L2 desc）：面板物理 PW x PH，
 *       GFX 显示层按生效旋转派生（奇数旋转交换宽高）——面板默认
 *       desc.gfx_rotation，可经 epd_set_rotation 运行期覆盖（屏幕
 *       方向设置，设置页即改即生效）；帧缓冲单平面 PW/8 x PH 字节
 *       （1bpp，bit=1 白），多平面色彩面板 x plane_count 连续布局
 *       （§9.3）
 */
#ifndef INKWORD_EPD_DRIVER_H
#define INKWORD_EPD_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "epd_panel.h"   /* L2 面板描述符：逻辑色 EPD_GFX_* 唯一权威定义处 */

#ifdef __cplusplus
extern "C" {
#endif

/* 双坐标体系：
 *   - 面板物理/底层直通：PW x PH（epd_full_refresh / epd_clear_screen）
 *   - GFX 显示层：生效旋转派生（epd_gfx_* 系列，UI 主路径）——
 *     desc.gfx_rotation 面板默认，epd_set_rotation 运行期覆盖
 * Phase 2 起 epd_driver 内部几何/帧长全部运行期取自 L2 desc（epd_panel.h）。
 * DEPG0370 兼容镜像宏 EPD_WIDTH/EPD_GFX_WIDTH 系列已于 Phase 6 删除：
 * 最后调用方 lan_display_server 同步动态化后全域零引用（铁律 2，
 * wifi_config_ui.c 范式），尺寸一律经 epd_gfx_width()/epd_fb_size() 等查询 */

/**
 * @brief 初始化墨水屏硬件：GPIO / SPI / 升压使能。
 * @return 0 成功；非 0 失败。
 */
int epd_driver_init(void);

/**
 * @brief 保留兼容的空操作：COG 收到 0x04 后自主升压，无需 MCU 干预。
 */
void epd_power_on(void);

/**
 * @brief 发送 0x02 让 COG 关闭高压 rails（VCI 3.3V 保持供电）。
 */
void epd_power_off(void);

/**
 * @brief 全屏清白（黑白交替深清：先全黑全刷再回白，洗掉陈年残影；
 *        开机白屏 / 清残影 / 局刷阈值等低频路径使用，多一次全刷）。
 */
void epd_clear_screen(void);

/**
 * @brief 全屏刷新（全刷模式，面板物理 PW x PH）。
 * @param data 面板物理整帧 1bpp 位图（epd_fb_total() 字节，行宽
 *             PW/8，bit=1 为白，与 COG SRAM 语义一致；多平面色彩
 *             面板为 plane_count 个平面连续布局，plane[1] 起 bit=1
 *             为红）。NULL 时等同于 epd_clear_screen()。
 */
void epd_full_refresh(const uint8_t *data);

/**
 * @brief 深度休眠（0x07/0xA5）。下次刷新前 GxEPD2 自动硬件复位并
 *        重新初始化 COG，无需重新调用 epd_driver_init()。
 */
void epd_deep_sleep(void);

/**
 * @brief 读取面板厂商与 ID（Phase 6 面板化：desc.name 下划线前段
 *        为厂商，返回 desc.controller 枚举值；保留诊断用途）。
 */
uint16_t epd_get_manufacturer(char *manufacturer, size_t len);

/**
 * @brief 运行期覆盖 GFX 旋转（屏幕方向设置，2026-08-26）。
 * @param rot {0,1,2,3}：奇数交换宽高（对应 GxEPD2 setRotation 语义，
 *        转置方向见 transpose_to_plane 四方向表）。
 * @return 0 成功（含与当前一致的幂等空操作）；-1 未初始化/参数非法/
 *         新画布分配失败（失败时原画布完好，渲染不受损）。
 *
 * 重建双层画布（GFXcanvas1 尺寸构造期固定）；帧缓冲按面板物理几何
 * 分配与旋转无关不重分配，s_port_prev 物理帧快照保持有效（屏幕物理
 * 内容未变）——新几何首次绘制由调用方全刷（main ui_apply_rotation
 * 统一失效布局缓存后重绘）。
 */
int epd_set_rotation(uint8_t rot);

/** @brief 当前生效旋转（未初始化返回 0）。 */
uint8_t epd_get_rotation(void);

/** @brief 面板默认旋转（desc.gfx_rotation 透传，未初始化返回 0）。 */
uint8_t epd_panel_default_rotation(void);

#ifdef __cplusplus
}
#endif

/* ============================================================
 * C-callable GFX 包装函数（供 .c 文件使用）
 * 基于内部 1bpp 帧缓冲的简易绘图 API
 * ============================================================ */
#ifdef __cplusplus
extern "C" {
#endif

/** @brief 获取帧缓冲宽度 */
int epd_gfx_width(void);
/** @brief 获取帧缓冲高度 */
int epd_gfx_height(void);
/** @brief 填充整个屏幕 (0=白, 1=黑) */
void epd_gfx_fill_screen(uint16_t color);
/** @brief 填充矩形 */
void epd_gfx_fill_rect(int x, int y, int w, int h, uint16_t color);
/** @brief 画矩形边框 */
void epd_gfx_draw_rect(int x, int y, int w, int h, uint16_t color);
/** @brief 画水平线 */
void epd_gfx_draw_hline(int x, int y, int w, uint16_t color);
/** @brief 画垂直线 */
void epd_gfx_draw_vline(int x, int y, int h, uint16_t color);
/** @brief 切换 FreeSans 常规/Bold 字体表（2026-08-27 P2 设置「粗细」）：
 *         draw_text/text_bounds 每次调用经 font_for_size 选表，切换
 *         即时生效且量测/绘制一致；仅 ASCII 路径生效，CJK 点阵不受影响 */
void epd_gfx_set_bold(bool on);
/** @brief 画文本（font_size: 1=小9pt, 2=中14pt默认, 3=大18pt, 4=特大24pt；y 为基线） */
void epd_gfx_draw_text(int x, int y, const char *text, uint16_t color, int font_size);
/** @brief 测量文本宽高 */
void epd_gfx_text_bounds(const char *text, int font_size, int *out_w, int *out_h);
/** @brief 绘制单色位图（Adafruit GFX 行主序 MSB-first，bit=1 画 color，0 保持背景；
 *         即 image2cpp "horizontal, MSB first" 导出格式，尺寸建议 8 对齐） */
void epd_gfx_draw_bitmap(int x, int y, int w, int h, const uint8_t *bits, uint16_t color);
/** @brief 读回画布窗口位图（与 draw_bitmap 同格式：行主序 MSB-first，
 *         bit=1=画布置位=黑），供上层新旧帧差分统计（智能局刷/全刷分流）；
 *         out 容量需 >= ceil(w/8)*h 字节 */
void epd_gfx_read_window(int x, int y, int w, int h, uint8_t *out);
/** @brief 将帧缓冲推送到屏幕（全刷） */
void epd_gfx_flush(void);
/** @brief 将指定区域推送到屏幕（局刷，默认遍数取面板 desc.passes：
 *         DEPG0370=2 双刷保净 / wft0290=1 单刷——双刷在单相 LUT 面板
 *         上实测产生过驱动伪影，见 panel_wft0290.cpp 调优史） */
void epd_gfx_flush_window(int x, int y, int w, int h);
/** @brief 同上，可指定同会话 0x12 次数（passes=1 单刷最快；多相波形
 *         面板单刷即净，双刷仅作浅影回退手段——注意单相 LUT 面板
 *         双刷会产生同向过驱动伪影，慎用） */
void epd_gfx_flush_window_passes(int x, int y, int w, int h, int passes);

/** @brief 局刷支持查询（desc.partial_enabled 透传）：三色面板等无
 *         快速局刷的面板返回 false，上层据此做 UX 降级（如待机页
 *         三色屏自动轮换停用，§13.2） */
bool epd_gfx_partial_supported(void);
/** @brief 当前面板 desc 指针（未初始化 NULL）：上层读取刷新策略
 *         字段（如 partial_count_full_refresh 保养阈值，避免再
 *         硬编码与 desc 脱钩） */
const epd_panel_desc_t *epd_panel_desc(void);
/** @brief 单平面帧字节数（ceil(panel_w/8) x panel_h；行宽向上取整，
 *         非 8 整除宽面板如 OPM021EB 122px → 16B/行；LAN 上传协议帧大小） */
size_t epd_fb_size(void);
/** @brief 全平面整帧字节数（epd_fb_size() x plane_count；外部直刷
 *         缓冲容量，多平面色彩面板含 accent 平面（红/黄）） */
size_t epd_fb_total(void);
/** @brief 面板物理宽（像素；LAN 上传页画布尺寸注入用） */
int epd_panel_width(void);
/** @brief 面板物理高（像素） */
int epd_panel_height(void);
/** @brief 第三色 RGB（desc.accent_rgb 透传：红屏 0xFF0000 /
 *         BW 面板 0；LAN 上传页量化调色板
 *         注入用，未初始化退 0） */
uint32_t epd_panel_accent_rgb(void);

#ifdef __cplusplus
}
#endif

/* GFX 颜色常量 EPD_GFX_BLACK/WHITE 已迁至 epd_panel.h（Phase 1 逻辑色
 * 唯一权威定义，值不变；ACCENT/AUX 为 Phase 6 色彩基建预留） */

#endif /* INKWORD_EPD_DRIVER_H */
