/**
 * @file selftest_frame.c
 * @brief T2.2 黄金帧回归自检实现（INKWORD_GOLDEN_FRAME 门控）
 *
 * 页面集（样板页选取原则 = 渲染结果确定性可复现）：
 *   - word_card：ui_render_word(MODE_FLASH, 0)——demo env 假词库
 *     index 0 恒定；须在任何 overlay 激活前渲染（守卫互斥）
 *   - settings：settings_ui_enter() 幂等自绘，零动态项（无时钟/
 *     电量/IP，grep 验证）；NVS 设置状态参与帧（换设备/擦 flash
 *     须重 dump 基线，见 selftest_golden.h 风险联动）
 *   - standby（自治钟/天气缓存）与 menu（学习统计 badge）含动态
 *     元素，整帧基准不可复现——二期区域 mask 后纳入
 *
 * 输出契约（tools/gen_golden.py 解析锚点，勿改格式）：
 *   [GOLDEN] BEGIN <page> <w> <h>
 *   [GOLDEN] B64 <64 字符>...   （48 字节/块，base64 标准 padding）
 *   [GOLDEN] END <page>
 * dump 行走 printf 裸通道（无 esp_log 前缀污染，脚本解析最稳），
 * 状态行走 LOG_I。
 *
 * 跑完 vTaskSuspend 挂起：demo 自检固件单用途，末帧驻屏供人工
 * 目视对照（对照本自检的"人工确认"步骤）。
 */
#if INKWORD_GOLDEN_FRAME

#include "epd_driver.h"
#include "study_mode_machine.h"
#include "settings_ui.h"
#include "debug_log.h"
#include "selftest_diff.h"
#include "selftest_golden.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "SELFTEST";

extern void ui_render_word(study_mode_t mode, int index); /* main.cpp */
extern const char *fw_version(void);                      /* main.cpp */

static int s_n_pass, s_n_fail, s_n_nobase;

/* ---- base64（标准字母表 + padding；仅 dump 单向编码） ---- */
static void b64_encode(const uint8_t *src, size_t n, char *dst)
{
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)src[i] << 16;
        if (i + 1 < n) v |= (uint32_t)src[i + 1] << 8;
        if (i + 2 < n) v |= src[i + 2];
        dst[o++] = tab[(v >> 18) & 63];
        dst[o++] = tab[(v >> 12) & 63];
        dst[o++] = (i + 1 < n) ? tab[(v >> 6) & 63] : '=';
        dst[o++] = (i + 2 < n) ? tab[v & 63] : '=';
    }
    dst[o] = 0;
}

/* 整帧 base64 dump（48B/块 = 64 字符/行，gen_golden.py 契约） */
static void dump_frame(const char *page, int w, int h,
                       const uint8_t *buf, size_t len)
{
    printf("[GOLDEN] BEGIN %s %d %d\n", page, w, h);
    char line[66];
    for (size_t off = 0; off < len; off += 48) {
        size_t n = (len - off < 48) ? len - off : 48;
        b64_encode(buf + off, n, line);
        printf("[GOLDEN] B64 %s\n", line);
    }
    printf("[GOLDEN] END %s\n", page);
}

#if GOLDEN_COUNT > 0
static const uint8_t *golden_find(const char *page, size_t *len)
{
    for (int i = 0; i < GOLDEN_COUNT; i++) {
        if (strcmp(k_golden[i].page, page) == 0) {
            *len = k_golden[i].len;
            return k_golden[i].frame;
        }
    }
    *len = 0;
    return NULL;
}
#endif

/* 与基准 diff：无基准→NO BASELINE（dump 已输出，供生成基线）；
 * 有基准→PASS / FAIL（差异字节数 + 首异位置行列，行粒度定位到
 * layout_profile 哪条参数动了——计划 T2.2 灵敏度验证口径 1 字节） */
static void check_page(const char *page, const uint8_t *buf, size_t len,
                       int stride)
{
#if GOLDEN_COUNT > 0
    size_t glen;
    const uint8_t *g = golden_find(page, &glen);
    if (!g) {
        s_n_nobase++;
        LOG_I("%s: NO BASELINE (dump above, run gen_golden.py)", page);
        return;
    }
    if (glen != len) {
        s_n_fail++;
        LOG_E("%s: FAIL (baseline %uB != frame %uB — 面板/布局档变更？)",
              page, (unsigned)glen, (unsigned)len);
        return;
    }
    long off;
    int n = selftest_diff_bytes(g, buf, len, &off);
    if (n == 0) {
        s_n_pass++;
        LOG_I("%s: PASS (diff 0B)", page);
    } else {
        s_n_fail++;
        LOG_E("%s: FAIL (diff %dB first @row %ld col 0x%02lX)",
              page, n, off / stride, off % stride);
    }
#else
    (void)buf; (void)stride;
    s_n_nobase++;
    LOG_I("%s: NO BASELINE (dump above, run gen_golden.py)", page);
#endif
}

/* 渲染一页 → 回读画布 → dump + 判定 */
static void run_page(const char *page, int w, int h,
                     uint8_t *buf, size_t len, void (*render)(void))
{
    LOG_I("page: %s (rendering...)", page);
    render();
    epd_gfx_read_window(0, 0, w, h, buf);
    dump_frame(page, w, h, buf, len);
    check_page(page, buf, len, (w + 7) / 8);
}

/* word_card 渲染闭包参数（MODE_FLASH, index 0）固定写死 */
static void render_word_card(void)
{
    ui_render_word(MODE_FLASH, 0);
}

void selftest_frame_run(void)
{
    const int w = epd_gfx_width(), h = epd_gfx_height();
    const size_t len = (size_t)((w + 7) / 8) * h;

    LOG_I("=== golden frame selftest start (panel=%s gfx=%dx%d fw=%s "
          "stride=%dB/frame=%uB) ===",
          epd_panel_desc()->name, w, h, fw_version(),
          (w + 7) / 8, (unsigned)len);

    uint8_t *buf = malloc(len);
    if (!buf) {
        LOG_E("frame buffer alloc failed (%uB)", (unsigned)len);
        vTaskSuspend(NULL);
        return;
    }

    /* 序列：word_card 须先于 settings（ui_render_word 守卫查
     * settings 激活态，反序会静默跳过渲染） */
    run_page("word_card", w, h, buf, len, render_word_card);
    run_page("settings", w, h, buf, len, settings_ui_enter);

    free(buf);
    LOG_I("=== selftest done: %d PASS / %d FAIL / %d NO-BASELINE "
          "(末帧驻屏对照；demo 自检固件挂起不进 loop) ===",
          s_n_pass, s_n_fail, s_n_nobase);
    vTaskSuspend(NULL);
}

#endif /* INKWORD_GOLDEN_FRAME */
