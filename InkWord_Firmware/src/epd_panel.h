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
    EPD_CTRL_JD79686, EPD_CTRL_SSD1619, EPD_CTRL_UC8151,
    EPD_CTRL_UNKNOWN,
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
    uint8_t             gfx_rotation;       /* 面板默认 UI 旋转 {0,1,2,3}：
                                             * 奇数=(panel_h,panel_w)，偶数直通；
                                             * 运行期可经 epd_set_rotation 覆盖
                                             * （屏幕方向设置），desc 本身 const */
    uint16_t            dpi;                /* 对角 PPI（诊断字段，2026-09-03：
                                             * 不参与档位/布局计算；上机对照
                                             * 「物理字高 mm = px÷dpi×25.4」
                                             * 判同档异 PPI 视觉风险，如
                                             * 4.2" 119 vs 3.4" 150 同为 MID） */

    /* —— 色彩 —— */
    epd_color_mode_t    color_mode;
    uint8_t             plane_count;    /* BW=1 / 3C,4C=2 / 6C=2~3 */
    uint8_t             palette[16];    /* 逻辑色→平面位掩码（§9.2） */
    uint32_t            accent_rgb;     /* 第三色 RGB（多平面面板：红屏
                                         * 0xFF0000；BW 面板缺省 0。
                                         * LAN 上传页量化调色板注入源 */
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
        void (*diag)(void);                         /* bring-up 状态读诊断
                                                     * （T1.8：族标准实现见
                                                     * epd_bus.h bus_diag_uc /
                                                     * bus_diag_ssd16；无 FLG/
                                                     * 版本寄存器的控制器填
                                                     * NULL 跳过。L3 只调不
                                                     * 判族，铁律 3） */
    } ops;
} epd_panel_desc_t;

/* 编译期默认面板 id：面板 env 直接注入 -D EPD_PANEL_DEFAULT_ID="..."
 * （platformio.ini；原 INKWORD_PANEL_* 中转宏链裁剪，P3 注册表收敛：
 * 加面板只改 platformio.ini env + panels/ 注册单元，本头不再维护
 * 型号清单）；未注入（统一固件 inkword-s3 / probe env）时兜底
 * DEPG0370，运行期 NVS set_panel 覆盖优先于本默认 */
#ifndef EPD_PANEL_DEFAULT_ID
#define EPD_PANEL_DEFAULT_ID "depg0370_uc8253"  /* 默认：3.7" 240x416 BW */
#endif

/**
 * @brief 运行期查表取面板描述符（量产预留 NVS 覆盖入口）。
 * @param id 面板注册名（desc.name，如 EPD_PANEL_DEFAULT_ID）。
 * @return 命中返回描述符指针（静态生存期）；未命中返回 NULL。
 */
const epd_panel_desc_t *epd_panel_get_by_id(const char *id);

/**
 * @brief 注册表面板总数（P1 运行期选屏：设置页「面板型号」行循环
 *        选择 / device-info 列举消费）。
 */
int epd_panel_registry_count(void);

/**
 * @brief 按注册序取面板描述符（与 get_by_id 同源注册表）。
 * @param idx 注册序号，0 <= idx < epd_panel_registry_count()。
 * @return 越界返回 NULL。
 */
const epd_panel_desc_t *epd_panel_at(int idx);

/**
 * @brief 面板描述符契约校验（P3，2026-09-05）。
 *
 * 字段间约束静态化（此前靠人工 review）：几何范围/帧预算/色彩平面
 * 匹配/调色板掩码/时序下限/ops 必填项。epd_driver_init 开机对全
 * 注册表跑一遍（违规 LOG_W，未选中屏也暴露），选中面板额外
 * fail-fast（desc 违规属构建期错误，拒绝带病初始化）。纯函数无
 * 硬件依赖，新屏接入后首个启动即自检 desc 笔误。
 *
 * @param d 待校验描述符（NULL 视为违规）。
 * @param err 违规描述输出缓冲（可 NULL：仅判成败）。
 * @param err_len err 容量（截断安全）。
 * @return 0 契约满足；-1 违规（err 填首个命中项）。
 */
int epd_panel_desc_check(const epd_panel_desc_t *d, char *err, size_t err_len);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_EPD_PANEL_H */
