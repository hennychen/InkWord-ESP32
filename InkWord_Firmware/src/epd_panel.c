/**
 * @file epd_panel.c
 * @brief L2 面板注册表 —— epd_panel_get_by_id() 查表实现
 *
 * 注册表为静态数组显式拼装（PANEL_COMPAT_DESIGN §5.3「链接期拼装静态
 * 注册表」的工程化落点）：未采用链接器 section 自动收集 —— ESP-IDF
 * 默认链接脚本无对应 KEEP 规则，--gc-sections 会回收无根引用的注册指针。
 * 新增面板三步：
 *   1. panels/xxx.cpp 定义 const epd_panel_desc_t（含 ops）；
 *   2. 此处 extern 声明；
 *   3. s_registry[] 追加一行。
 * Phase 3 构建矩阵落地：面板单元当前均无条件编入（S3 16MB Flash
 * 充裕，注册表完整保留运行期查表能力）；面板 env 经 -D EPD_PANEL_DEFAULT_ID 直钉默认（P3 宏链裁剪），
 * 未选单元的编译期排除留作 Flash 紧张时的优化项。P1 收官（2026-09-02）：NVS set_panel 运行期
 * 选屏落地（epd_driver_init 读键覆盖默认 ID，详见该处注释），
 * 本文件枚举 API（registry_count/at）供设置页循环选择使用
 */
#include "epd_panel.h"

#include <stdio.h>
#include <string.h>

/* panels/panel_depg0370_uc8253.cpp（3.7" BW，UC8253） */
extern const epd_panel_desc_t g_panel_depg0370;
/* panels/panel_e042a13_ssd1619.cpp（4.2" BWR 三色，SSD1619 手写序列，
 * 2026-08-22 真机勘误：初判 IL0398 有误，IC 实为 SSD1619） */
extern const epd_panel_desc_t g_panel_e042a13;
/* panels/panel_wf0270_ssd1680.cpp（2.7" BWR 三色，SSD1680 手写序列，
 * 22Pin 0.5mm FPC，维峰 WEIFENG WF0270T1PCZ2200E4；到货前未经真机） */
extern const epd_panel_desc_t g_panel_wf0270;
/* panels/panel_gdew027c44_il91874.cpp（2.7" BWR 三色，IL91874 手写序列，
 * 24Pin；2026-08-22 真机 bring-up 完成：原判 WF0270 型号有误，实物
 * IL91874/EK79652 家族，GxEPD2_270c 序列点亮，全刷 14720ms） */
extern const epd_panel_desc_t g_panel_gdew027c44;
/* 以下两块 2026-08-23 新增骨架（屏在途）：几何/档位已定，驱动序列
 * 待真机 bring-up（§十六 SOP）；ops 为 fail-safe 桩（init 拒绝） */
/* panels/panel_wft0290.cpp（2.9" 128x296 BW 竖屏，WFT0290CZ10，
 * LAYOUT_TINY 档；控制器 UC8253/SSD1680 待实测判定） */
extern const epd_panel_desc_t g_panel_wft0290;
/* panels/panel_opm021eb.cpp（2.13" 122x250 BW 竖屏，OPM021EB 电子
 * 标签，LAYOUT_TINY 档；控制器疑似 SSD1680 待实证） */
extern const epd_panel_desc_t g_panel_opm021eb;
/* panels/panel_e042a13bw.cpp（4.2" 400x300 BW，HINK-E042A13-A0 黑白
 * 版，LAYOUT_MID 档；SSD1619，三色兄弟屏同族，2026-08-30 bring-up 完成） */
extern const epd_panel_desc_t g_panel_e042a13bw;
/* panels/panel_310_uc8253.cpp（3.1" 320x240 BW，UC8253，24P FPC 0.5mm，
 * 2026-09-05 新增：骨架取自 DEPG0370 UC8253 同族，PSR 方向待真机标定） */
extern const epd_panel_desc_t g_panel_310;
/* panels/panel_hink_e0213a31.cpp（2.13" 122x250 BW 竖屏，HINK-E0213A31-A0，
 * LAYOUT_TINY 档；SSD1680，GxEPD2 B74 序列，2026-09-05 新增） */
extern const epd_panel_desc_t g_panel_hink_e0213a31;

static const epd_panel_desc_t *const s_registry[] = {
    &g_panel_depg0370,
    &g_panel_e042a13,
    &g_panel_wf0270,
    &g_panel_gdew027c44,
    &g_panel_wft0290,
    &g_panel_opm021eb,
    &g_panel_e042a13bw,
    &g_panel_310,
    &g_panel_hink_e0213a31,
};

const epd_panel_desc_t *epd_panel_get_by_id(const char *id)
{
    if (!id) return NULL;
    for (size_t i = 0; i < sizeof(s_registry) / sizeof(s_registry[0]); i++) {
        if (strcmp(s_registry[i]->name, id) == 0) {
            return s_registry[i];
        }
    }
    return NULL;
}

int epd_panel_registry_count(void)
{
    return (int)(sizeof(s_registry) / sizeof(s_registry[0]));
}

const epd_panel_desc_t *epd_panel_at(int idx)
{
    if (idx < 0 || (size_t)idx >= sizeof(s_registry) / sizeof(s_registry[0]))
        return NULL;
    return s_registry[idx];
}

/* ---- P3 desc 契约校验（字段间约束静态化，2026-09-05）----
 * 判定顺序 ≈ desc 字段声明序，首错即返。帧预算 512KB：现役最大
 * 7.5" 800x480 双平面 ~94KB，取 5 倍余量作契约天花板（再大基本
 * 必为几何字段笔误）。dpi=0 是除零风险（epd_driver_init 诊断行
 * 25.4f/dpi），不是纯诊断项 */
#define DESC_FAIL(fmt, ...) do {                                            \
    if (err && err_len)                                                     \
        snprintf(err, err_len, fmt, __VA_ARGS__);                           \
    return -1;                                                              \
} while (0)

int epd_panel_desc_check(const epd_panel_desc_t *d, char *err, size_t err_len)
{
    if (!d || !d->name || !d->name[0])
        DESC_FAIL("%s", "bad name");
    if (d->panel_w < 16 || d->panel_w > 2048 ||
        d->panel_h < 16 || d->panel_h > 4096)
        DESC_FAIL("geometry %ux%u out of range",
                  (unsigned)d->panel_w, (unsigned)d->panel_h);

    const size_t stride = ((size_t)d->panel_w + 7) / 8;
    if (stride * d->panel_h * d->plane_count > 512u * 1024u)
        DESC_FAIL("frame budget %uB over 512KB",
                  (unsigned)(stride * d->panel_h * d->plane_count));

    if (d->gfx_rotation > 3)
        DESC_FAIL("gfx_rotation=%u > 3", (unsigned)d->gfx_rotation);

    switch (d->color_mode) {
    case EPD_COLOR_BW:
        if (d->plane_count != 1)
            DESC_FAIL("BW plane_count=%u != 1", (unsigned)d->plane_count);
        break;
    case EPD_COLOR_3C:
    case EPD_COLOR_4C:
        if (d->plane_count != 2)
            DESC_FAIL("3C/4C plane_count=%u != 2", (unsigned)d->plane_count);
        break;
    case EPD_COLOR_6C:
        if (d->plane_count < 2 || d->plane_count > 3)
            DESC_FAIL("6C plane_count=%u not in {2,3}",
                      (unsigned)d->plane_count);
        break;
    default:
        DESC_FAIL("bad color_mode=%d", (int)d->color_mode);
    }

    /* 调色板掩码：每 bit 对应一平面，超过 plane_count 位即越平面 */
    for (int i = 0; i < 16; i++)
        if (d->palette[i] >= (uint8_t)(1u << d->plane_count))
            DESC_FAIL("palette[%d]=0x%02x over plane_count=%u", i,
                      (unsigned)d->palette[i], (unsigned)d->plane_count);

    if (d->dpi == 0)
        DESC_FAIL("%s", "dpi=0 (div-by-zero in epd_driver_init)");
    if (d->rst_pulse_ms == 0)
        DESC_FAIL("%s", "rst_pulse_ms=0");
    if (d->busy_level > 1)
        DESC_FAIL("busy_level=%u > 1", (unsigned)d->busy_level);
    if (d->busy_timeout_ms < 100)
        DESC_FAIL("busy_timeout_ms=%u < 100",
                  (unsigned)d->busy_timeout_ms);
    if (d->passes < 1)
        DESC_FAIL("%s", "passes=0");
    if (d->partial_count_full_refresh < 1)
        DESC_FAIL("%s", "partial_count_full_refresh=0");

    if (!d->ops.init || !d->ops.full_refresh || !d->ops.write_full ||
        !d->ops.power_off || !d->ops.deep_sleep)
        DESC_FAIL("%s", "missing mandatory op(s)");
    if (d->partial_enabled && !d->ops.partial)
        DESC_FAIL("%s", "partial_enabled but ops.partial NULL");

    return 0;
}

#undef DESC_FAIL
