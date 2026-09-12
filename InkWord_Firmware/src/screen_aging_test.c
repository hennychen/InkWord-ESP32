/**
 * @file screen_aging_test.c
 * @brief 屏幕老化诊断测试（2026-09-10 v3）
 *
 * 针对 OPM021EB（UC8151D OTP LUT 模式）的屏幕诊断工具。
 *
 * 关键认知：PSR 0x1F OTP LUT 模式下，VCOM(0x82)/CDI(0x50) 寄存器
 * 被 COG 忽略（二十二轮实证），波形能量烧死在 OTP 里。
 * 因此 VCOM/CDI 扫描无效，替换为打断时间扫描（唯一可调参数）。
 *
 * 测试项目：
 * 0. 全白测试 - 检查均匀性
 * 1. 全黑测试 - 检查均匀性
 * 2. 九宫格测试 - 分区定位老化区域
 * 3. 打断时间扫描 - 500/750/1000/1500/2000/2970ms 六档（带屏幕标记）
 * 4. 快速翻页残影测试 - 连续局刷检查残影积累
 * 5. 全刷 vs 打断法对比 - 全刷 3.2s vs 打断法效果对比
 */

#include "screen_aging_test.h"
#include "epd_driver.h"
#include "epd_panel.h"
#include "panels/epd_bus.h"
#include "gpio_config.h"

#include <Arduino.h>
#include <string.h>
#include <stdio.h>
#include "debug_log.h"   /* 必须在 Arduino.h 之后 */

#define TAG "AGING_TEST"

/* 打断时间覆盖变量（定义在 panel_opm021eb.cpp） */
extern volatile uint16_t g_abort_ms_override;

/* 前置声明 */
static void test_full_white(void);
static void test_full_black(void);
static void test_grid_3x3(void);
static void test_abort_sweep(void);
static void test_ghosting(void);
static void test_full_vs_abort(void);

/* 测试状态 */
static bool s_test_active = false;
static int s_test_index = 0;

/* 测试项列表 */
typedef void (*test_func_t)(void);
static const test_func_t s_tests[] = {
    test_full_white,       /* 0: 全白均匀性 */
    test_full_black,       /* 1: 全黑均匀性 */
    test_grid_3x3,         /* 2: 九宫格分区 */
    test_abort_sweep,      /* 3: 打断时间扫描 */
    test_ghosting,         /* 4: 快速翻页残影 */
    test_full_vs_abort,    /* 5: 全刷 vs 打断对比 */
};
#define TEST_COUNT (sizeof(s_tests) / sizeof(s_tests[0]))

/* ========== 测试 0: 全白均匀性 ========== */
static void test_full_white(void)
{
    LOG_I("=== TEST 0: Full White Uniformity ===");
    LOG_I("观察：中间 vs 四周边缘亮度一致性");
    LOG_I("老化特征：中心区域显灰色");

    epd_gfx_fill_screen(EPD_GFX_WHITE);
    epd_gfx_flush();

    LOG_I("全白显示完成，按任意键切换");
}

/* ========== 测试 1: 全黑均匀性 ========== */
static void test_full_black(void)
{
    LOG_I("=== TEST 1: Full Black Uniformity ===");
    LOG_I("观察：中间区域是否不够黑（发灰）");
    LOG_I("老化特征：中心黑色不纯，对比度下降");

    epd_gfx_fill_screen(EPD_GFX_BLACK);
    epd_gfx_flush();

    LOG_I("全黑显示完成，按任意键切换");
}

/* ========== 测试 2: 九宫格分区 ========== */
static void test_grid_3x3(void)
{
    const int w = epd_gfx_width();
    const int h = epd_gfx_height();
    const int cw = w / 3;
    const int ch = h / 3;

    LOG_I("=== TEST 2: 3x3 Grid Zone ===");
    LOG_I("分区: %dx%d -> 每区 %dx%d px", w, h, cw, ch);
    LOG_I("棋盘格：偶数位黑/奇数位白");

    epd_gfx_fill_screen(EPD_GFX_WHITE);

    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            int x = col * cw;
            int y = row * ch;
            if ((row + col) % 2 == 0) {
                epd_gfx_fill_rect(x, y, cw, ch, EPD_GFX_BLACK);
            }
        }
    }

    /* 分区编号 */
    char label[8];
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            int x = col * cw + cw / 2 - 4;
            int y = row * ch + ch / 2 - 4;
            snprintf(label, sizeof(label), "%d", row * 3 + col + 1);
            epd_gfx_draw_text(x, y, label,
                ((row + col) % 2 == 0) ? EPD_GFX_WHITE : EPD_GFX_BLACK, 1);
        }
    }

    epd_gfx_flush();

    LOG_I("九宫格显示完成，按任意键切换");
}

/* ========== 测试 3: 打断时间扫描（带屏幕标记） ========== */
static void test_abort_sweep(void)
{
    const int w = epd_gfx_width();
    const int h = epd_gfx_height();

    LOG_I("=== TEST 3: Abort Timing Sweep (with marker) ===");
    LOG_I("六档扫描，每档屏幕显示当前打断时间");
    LOG_I("观察：哪档白底最白、黑字最清晰");

    /* 六档打断时间 */
    static const uint16_t abort_ms[] = {
        500, 750, 1000, 1500, 2000, 2970
    };
    const int count = sizeof(abort_ms) / sizeof(abort_ms[0]);

    for (int i = 0; i < count; i++) {
        LOG_I("Abort = %dms (%d/6)", abort_ms[i], i + 1);

        /* 设置打断时间覆盖 */
        g_abort_ms_override = abort_ms[i];

        /* 画测试帧：白底 + 黑字标记 + 黑色方块 */
        epd_gfx_fill_screen(EPD_GFX_WHITE);

        /* 顶部标记：当前打断时间 */
        char title[32];
        snprintf(title, sizeof(title), "Abort=%dms", abort_ms[i]);
        epd_gfx_draw_text(10, 10, title, EPD_GFX_BLACK, 2);

        /* 中部大字号标记 */
        char big_label[16];
        snprintf(big_label, sizeof(big_label), "%d/%d", i + 1, count);
        epd_gfx_draw_text(w / 2 - 20, h / 2 - 20, big_label, EPD_GFX_BLACK, 3);

        /* 底部黑色方块（测试黑色对比度） */
        epd_gfx_fill_rect(20, h - 60, w - 40, 40, EPD_GFX_BLACK);

        /* 局刷（使用 g_abort_ms_override 指定的打断时间） */
        epd_gfx_flush();

        delay(4000);  /* 留 4 秒观察 */
    }

    /* 恢复默认打断时间 */
    g_abort_ms_override = 0;

    LOG_I("打断扫描完成");
    LOG_I("时间越长 -> 白底越白、对比度越高");
    LOG_I("但时间越长 -> 闪烁越明显、翻页体验越差");
    LOG_I("按任意键切换");
}

/* ========== 测试 4: 快速翻页残影 ========== */
static void test_ghosting(void)
{
    const int w = epd_gfx_width();
    const int h = epd_gfx_height();

    LOG_I("=== TEST 4: Ghosting Test ===");
    LOG_I("连续 8 次局刷（白↔黑交替），观察残影积累");

    for (int i = 0; i < 8; i++) {
        bool is_black = (i % 2 == 0);

        epd_gfx_fill_screen(is_black ? EPD_GFX_BLACK : EPD_GFX_WHITE);

        /* 显示当前状态 */
        char label[32];
        snprintf(label, sizeof(label), "Flip %d/8 %s",
                 i + 1, is_black ? "BLACK" : "WHITE");
        epd_gfx_draw_text(10, h / 2 - 10, label,
                         is_black ? EPD_GFX_WHITE : EPD_GFX_BLACK, 2);

        epd_gfx_flush();
        delay(1500);
    }

    LOG_I("残影测试完成");
    LOG_I("观察：是否有前次内容残留（鬼影）");
    LOG_I("打断法截断清屏相位会积累残影");
    LOG_I("按任意键切换");
}

/* ========== 测试 5: 全刷 vs 打断法对比 ========== */
static void test_full_vs_abort(void)
{
    const int w = epd_gfx_width();
    const int h = epd_gfx_height();

    LOG_I("=== TEST 5: Full Refresh vs Abort ===");
    LOG_I("对比全刷（3.2s 完整 OTP 波形）vs 打断法（500ms）");

    /* A: 全刷黑底 */
    LOG_I("A: Full refresh -> BLACK (~3.2s)...");
    epd_gfx_fill_screen(EPD_GFX_BLACK);
    epd_gfx_draw_text(10, 10, "A: FULL", EPD_GFX_WHITE, 2);
    epd_gfx_draw_text(10, 40, "3200ms", EPD_GFX_WHITE, 2);
    epd_gfx_flush();  /* 全刷路径 */
    LOG_I("A: 全刷完成，观察黑色纯度（基准）");
    delay(4000);

    /* B: 打断法白底 */
    LOG_I("B: Abort refresh -> WHITE (500ms)...");
    g_abort_ms_override = 500;
    epd_gfx_fill_screen(EPD_GFX_WHITE);
    epd_gfx_draw_text(10, 10, "B: ABORT", EPD_GFX_BLACK, 2);
    epd_gfx_draw_text(10, 40, "500ms", EPD_GFX_BLACK, 2);
    epd_gfx_flush();  /* 局刷路径，使用 500ms 打断 */
    LOG_I("B: 打断法完成，与 A 对比白底纯度");
    delay(4000);

    /* 恢复默认 */
    g_abort_ms_override = 0;

    LOG_I("对比完成");
    LOG_I("A = 全刷基准（最高对比度，但闪烁 3.2s）");
    LOG_I("B = 打断法（快 ~0.9s，但对比度可能略低）");
    LOG_I("按任意键切换");
}

/* ========== 公共接口 ========== */

void screen_aging_test_enter(void)
{
    if (s_test_active) return;

    LOG_I("===========================================");
    LOG_I("  屏幕老化诊断测试 v3.0 (带屏幕标记)");
    LOG_I("===========================================");
    LOG_I("测试项目 (%d 项):", TEST_COUNT);
    LOG_I("  0: 全白均匀性");
    LOG_I("  1: 全黑均匀性");
    LOG_I("  2: 九宫格分区");
    LOG_I("  3: 打断时间扫描（带屏幕标记）");
    LOG_I("  4: 快速翻页残影");
    LOG_I("  5: 全刷 vs 打断法对比");
    LOG_I("===========================================");
    LOG_I("注意: OTP 模式下 VCOM/CDI 寄存器被忽略");
    LOG_I("短按任意键: 切换测试项");

    s_test_active = true;
    s_test_index = 0;

    s_tests[s_test_index]();
}

void screen_aging_test_exit(void)
{
    if (!s_test_active) return;
    g_abort_ms_override = 0;  /* 恢复默认 */
    LOG_I("退出屏幕老化诊断测试");
    s_test_active = false;
    s_test_index = 0;
}

bool screen_aging_test_on_button(int key_id, int event)
{
    if (!s_test_active) return false;

    if (event == 1) {  /* 短按 */
        s_test_index = (s_test_index + 1) % TEST_COUNT;
        LOG_I("切换到测试 %d/%d", s_test_index + 1, TEST_COUNT);
        s_tests[s_test_index]();
        return true;
    }

    return true;
}

bool screen_aging_test_is_active(void)
{
    return s_test_active;
}
