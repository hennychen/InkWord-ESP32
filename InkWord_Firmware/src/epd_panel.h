/**
 * @file epd_panel.h
 * @brief L2 面板描述符 —— 多屏兼容架构核心数据结构
 *        （PANEL_COMPAT_DESIGN.md §五）
 *
 * 面板差异（几何/色彩/时序/刷新策略/控制器序列入口）全部字段化于
 * epd_panel_desc_t；L3（epd_driver）持有唯一 desc 指针经 ops 调用，
 * 不感知具体面板型号（铁律 3：驱动差异封装进 panels/ 独立文件）。
 *
 * 首个注册单元：panels/panel_depg0370_uc8253.cpp（Phase 1，纯重构迁入）。
 * 首个色彩面板：panels/panel_e042a13_ssd1619.cpp（Phase 3+6，手写序列；
 * 2026-08-22 真机勘误：IC 实为 SSD1619，初判 IL0398 有误）。
 * 后续面板按 §十六 SOP 接入：定 desc → 选/写面板类 → probe 实测回填时序。
 */
#ifndef INKWORD_EPD_PANEL_H
#define INKWORD_EPD_PANEL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 逻辑色（§9.2/§9.4）：UI / epd_gfx_* 绘图 API 的色彩语义
 * WHITE=0 / BLACK=1 与既有 1bpp 取值一致（原 epd_driver.h 定义迁此，
 * 值不变，调用方零改动）；ACCENT/AUX 为 Phase 6 色彩基建预留，BW 面板
 * 经 desc.palette 退化映射为黑（黑白优先：不依赖颜色传达信息）
 * ============================================================ */
#define EPD_GFX_WHITE   0
#define EPD_GFX_BLACK   1
#define EPD_GFX_ACCENT  2   /* 强调色（待机出处/音标），Phase 6 启用 */
#define EPD_GFX_AUX     3   /* 次强调色，Phase 6 启用 */

typedef enum {
    EPD_CTRL_UC8253, EPD_CTRL_SSD1680, EPD_CTRL_SSD1681,
    EPD_CTRL_IL0398, EPD_CTRL_IL91874, EPD_CTRL_UC8179,
    EPD_CTRL_JD79686, EPD_CTRL_SSD1619, EPD_CTRL_UNKNOWN,
} epd_controller_t;   /* 色彩面板控制器在选型时按 SOP（§十六）核对 */

typedef enum { EPD_COLOR_BW, EPD_COLOR_3C, EPD_COLOR_4C, EPD_COLOR_6C }
    epd_color_mode_t;

typedef enum { EPD_FB_AUTO, EPD_FB_SRAM, EPD_FB_PSRAM } epd_fb_location_t;

typedef struct epd_panel_desc {
    /* —— 身份 —— */
    const char          *name;          /* "depg0370_uc8253"（注册查表主键） */
    epd_controller_t    controller;

    /* —— 几何 —— */
    uint16_t            panel_w, panel_h;   /* 物理竖屏分辨率 */
    uint8_t             gfx_rotation;       /* UI 横屏旋转 {0,1,2,3}：
                                             * 奇数=(panel_h,panel_w)，偶数直通 */

    /* —— 色彩 —— */
    epd_color_mode_t    color_mode;
    uint8_t             plane_count;    /* BW=1 / 3C,4C=2 / 6C=2~3 */
    uint8_t             palette[16];    /* 逻辑色→平面位掩码（§9.2） */
    epd_fb_location_t   fb_location;    /* 默认 AUTO：>128KB 阈值落 PSRAM（§十） */

    /* —— 时序 —— */
    uint16_t            rst_pulse_ms;
    uint8_t             busy_level;     /* BUSY 忙电平（0/1：UC8253 系 LOW
                                         * 忙 / SSD16xx 系 HIGH 忙） */
    uint32_t            busy_timeout_ms;/* 色彩面板 20~60s 各异，禁用全局默认 */
    uint16_t            power_on_ms, power_off_ms;
    uint32_t            full_ms, partial_ms;

    /* —— 刷新策略 —— */
    bool                partial_enabled;/* 色彩面板一律 false（§13.2） */
    uint8_t             passes;         /* 局刷默认遍数（调用方可覆盖） */
    uint8_t             partial_count_full_refresh; /* 局刷计数全刷阈值 */
    bool                window_8align;  /* 面板侧 x/w 8 像素对齐要求 */

    /* —— ops：L0 控制器序列统一入口 ——
     * 约定：各序列完全无状态（UC8253 双 RAM 差分先例：不依赖 COG 内部
     * RAM 跨调用存活）。Phase 1 实现前六项；probe / write_planes 为
     * probe 环境（§14.3）/ Phase 6 多平面（§9.3）预留，当前填 NULL */
    struct {
        int  (*init)(void);                         /* 上电初始化：复位+初始序列 */
        int  (*full_refresh)(const uint8_t *frame); /* UI 真全刷（demo 忠实序列） */
        int  (*write_full)(const uint8_t *frame);   /* 竖屏原始帧直通全刷（LAN；
                                                     * frame=NULL 时清白） */
        int  (*partial)(const uint8_t *prev, const uint8_t *new_,
                        uint8_t passes);            /* 无窗口双 RAM 差分局刷 */
        void (*power_off)(void);                    /* 0x02 关高压 rails */
        void (*deep_sleep)(void);                   /* 0x07/0xA5 深睡 */
        int  (*probe)(void);                        /* 读 MANUFACTURE_ID 探测 */
        int  (*write_planes)(const uint8_t *const *planes); /* 多平面统一入口 */
    } ops;
} epd_panel_desc_t;

/* Phase 3 构建矩阵：面板轴编译期选择（§7.1/§14.1，platformio.ini
 * env 注入 -D 宏；板级轴 INKWORD_BOARD_* 与此正交，二维自由组合）。
 * 新增面板：在此追加一个 elif 分支 + platformio.ini 对应 env */
#if defined(INKWORD_PANEL_E042A13)
#define EPD_PANEL_DEFAULT_ID "e042a13_ssd1619"  /* 4.2" 400x300 BWR 三色 */
#elif defined(INKWORD_PANEL_WF0270)
#define EPD_PANEL_DEFAULT_ID "wf0270_ssd1680"  /* 2.7" 264x176 BWR 三色 */
#elif defined(INKWORD_PANEL_GDEW027C44)
#define EPD_PANEL_DEFAULT_ID "gdew027c44_il91874"  /* 2.7" 264x176 BWR
                                                     * 三色（真机验证面板） */
#elif defined(INKWORD_PANEL_E042A13BW)
#define EPD_PANEL_DEFAULT_ID "e042a13bw_ssd1619"  /* 4.2" 400x300 BW
                              * （E042A13-A0 黑白版，骨架：bring-up 待硬件） */
#elif defined(INKWORD_PANEL_WFT0290)
#define EPD_PANEL_DEFAULT_ID "wft0290_bw"  /* 2.9" 128x296 BW 竖屏
                              * （WFT0290CZ10，骨架：bring-up 待硬件） */
#elif defined(INKWORD_PANEL_OPM021EB)
#define EPD_PANEL_DEFAULT_ID "opm021eb_bw"  /* 2.13" 122x250 BW 竖屏
                              * （电子标签，骨架：bring-up 待硬件） */
#else
#define EPD_PANEL_DEFAULT_ID "depg0370_uc8253"  /* 默认：3.7" 240x416 BW */
#endif

/**
 * @brief 运行期查表取面板描述符（量产预留 NVS 覆盖入口）。
 * @param id 面板注册名（desc.name，如 EPD_PANEL_DEFAULT_ID）。
 * @return 命中返回描述符指针（静态生存期）；未命中返回 NULL。
 */
const epd_panel_desc_t *epd_panel_get_by_id(const char *id);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_EPD_PANEL_H */
