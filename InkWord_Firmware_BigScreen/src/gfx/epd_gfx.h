/**
 * @file epd_gfx.h
 * @brief 大屏绘图层 —— 小屏 epd_gfx_* C API 的 epdiy 实现（迁移对接面）
 *
 * 架构（迁移 Phase A+B，2026-09-16）：小屏 InkWord_Firmware 的 UI/业务
 * 模块经 epd_gfx_* C API 绘图（本头与 epd_driver.h GFX 段同签名同语义），
 * 本模块为 epdiy 4bpp 帧缓冲管线的大屏实现——业务模块近零改动移植。
 *
 * 内部管线：PSRAM 1bpp 画布（bit=1 黑 / 0 白，与小屏 COG 语义一致）
 *   → flush 时按字节查表展开 4bpp（0=黑 15=白）→ epd_hl_update_screen
 *   MODE_GC16 全刷（~3-4s）；flush_window 走 panel 底层 DU 窗口局刷
 *   （run88 P4 路径，~0.5s 级；卖家 patch 库 hl 局刷已魔改退化全屏，
 *   见 panel_es108fc.h）——2026-09-16 实装，partial_supported() true。
 *   灰染纪律（§10.3/run91）：每 K=8 次局刷自动插入全屏 GC16 重置。
 *
 * 字体：FreeSans 四档（9/12/18/24pt）+ Bold 四档（fonts/，Adafruit GFX
 * 数据表）+ Helv 大字两档（36/48pt ± Bold，系统 Helvetica 生成
 * tools/gen_gfx_font.py，2026-09-17 UI 重设计）；draw_text y 语义 =
 * 文本基线（与小屏一致）。中文经 cjk_font 点阵（六级 16~48px，
 * app/core/，cjk_text 混排调用 draw_bitmap blit）。
 */
#ifndef BIGSCREEN_EPD_GFX_H
#define BIGSCREEN_EPD_GFX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 逻辑色（与小屏 epd_panel.h 同值：UI 绘图 API 色彩语义单一权威）。
 * ACCENT/AUX 为多色面板预留：大屏单色管线在 epd_geom 层退化映射
 * 为黑（与小屏 BW 面板同规则） */
#define EPD_GFX_WHITE 0
#define EPD_GFX_BLACK 1
#define EPD_GFX_ACCENT 2
#define EPD_GFX_AUX 3

/**
 * @brief 初始化绘图层：分配 PSRAM 1bpp 画布并整屏置白。
 *        前置：panel_es108fc_safe_init() 已成功（帧缓冲就绪）。
 * @return 0 成功；-1 画布分配失败。
 */
int epd_gfx_init(void);

/** @brief 画布宽（像素，X 方向） */
int epd_gfx_width(void);
/** @brief 画布高（像素，Y 方向） */
int epd_gfx_height(void);

/** @brief 填充整个画布 (EPD_GFX_WHITE=0 / EPD_GFX_BLACK=1)，不刷新 */
void epd_gfx_fill_screen(uint16_t color);
/** @brief 填充矩形（不刷新） */
void epd_gfx_fill_rect(int x, int y, int w, int h, uint16_t color);
/** @brief 画 1px 矩形边框（不刷新） */
void epd_gfx_draw_rect(int x, int y, int w, int h, uint16_t color);
/** @brief 画水平线（不刷新） */
void epd_gfx_draw_hline(int x, int y, int w, uint16_t color);
/** @brief 画垂直线（不刷新） */
void epd_gfx_draw_vline(int x, int y, int h, uint16_t color);

/** @brief 切换 FreeSans 常规/Bold 字体表（同小屏：仅 ASCII 路径生效） */
void epd_gfx_set_bold(bool on);
/**
 * @brief 画 ASCII 文本（font_size: 1=9pt, 2=12pt, 3=18pt, 4=24pt,
 *        5=36pt, 6=48pt（5/6 档为 Helv 表，2026-09-17）；
 *        y 为基线——与小屏 epd_gfx_draw_text 语义一致；非 ASCII 字符
 *        跳过不绘制，中文混排走 cjk_text）。
 */
void epd_gfx_draw_text(int x, int y, const char *text, uint16_t color,
                       int font_size);
/** @brief 测量文本包围盒（minx/miny 相对起点；含 xOffset/yOffset 修正） */
void epd_gfx_text_bounds(const char *text, int font_size, int *out_w,
                         int *out_h);

/**
 * @brief 绘制单色位图（Adafruit GFX 行主序 MSB-first，bit=1 画 color，
 *        0 保持背景；行字节对齐——cjk 点阵/draw_bitmap 生成器通用格式）。
 */
void epd_gfx_draw_bitmap(int x, int y, int w, int h, const uint8_t *bits,
                         uint16_t color);
/** @brief 读回画布窗口位图（同 draw_bitmap 格式；bit=1=画布置位=黑），
 *         out 容量需 >= ceil(w/8)*h 字节 */
void epd_gfx_read_window(int x, int y, int w, int h, uint8_t *out);

/** @brief 画布推送到屏幕（1bpp→4bpp 展开 + GC16 全刷，~3-4s） */
void epd_gfx_flush(void);
/** @brief 区域局部刷新（DU 窗口扫描 + 差异行跳过，~0.5s 级；
 *         每 K=8 次自动插入全屏 GC16 重置驱白灰染，run91 纪律） */
void epd_gfx_flush_window(int x, int y, int w, int h);
/** @brief 同 flush_window（passes 保 API 兼容被忽略） */
void epd_gfx_flush_window_passes(int x, int y, int w, int h, int passes);
/** @brief 强制全屏重驱：back 置黑构造全屏 diff → GC16 白区全驱驱白。
 *         清灰染/残影（ghost-clear 命令用）；不依赖内容变化 */
void epd_gfx_force_refresh(void);
/** @brief binfast 自驱深清（run115 序列移植）：白→黑→白→画布内容四段
 *         全驱（~3s）。较 force_refresh 多两轮满摆幅往返，可清除 DU
 *         局刷灰染与深层滞留；局刷 K 重置自动调用，也可手动触发 */
void epd_gfx_deep_clean(void);

/** @brief 全刷波形选择（2026-09-17）：true=binfast 二值快速——黑白跃迁
 *         各 bn 扫（默认 3+3≈0.7s，vs GC16 30 相位≈3.3s），同色 nop 零
 *         注入→翻转少边界扩散小→更锐；false=GC16 builtin 原波形。
 *         仅作用于 flush/force_refresh 全刷路径（DU 局刷波形不受影响）。
 *         本画布 1bit 内容全场景适用；真机迭代通道：串口命令 w 切换。 */
void epd_gfx_set_binfast(bool on);
/** @brief 全刷波形当前选择（status/诊断回显用） */
bool epd_gfx_binfast_enabled(void);

/** @brief 局刷支持查询：true（DU 窗口局刷已实装） */
bool epd_gfx_partial_supported(void);

#ifdef __cplusplus
}
#endif
#endif /* BIGSCREEN_EPD_GFX_H */
