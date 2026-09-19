// 波形调参测试任务实现
// 标准测试图 + 运行时参数切换，用于系统化扫描 binfast/scanq 最优参数
#include "tune/tune_task.h"

#include <stdio.h>
#include <string.h>

#include "driver/panel_es108fc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gfx/epd_gfx.h"
#include "lan/waveform_scanq.h"

static const char* TAG = "tune";

#define W 1920
#define H 1080

// 参数组合预设（binfast n1+n2, scanq nsat+mmax）
typedef struct {
    const char* name;
    int bf_n1, bf_n2;
    int sq_nsat, sq_mmax;
} TunePreset;

static const TunePreset s_presets[] = {
    {"BF 2+2", 2, 2, 10, 5},
    {"BF 3+3", 3, 3, 10, 5},
    {"BF 4+4", 4, 4, 10, 5},
    {"BF 5+5", 5, 5, 10, 5},
    {"BF 3+5", 3, 5, 10, 5},
    {"BF 5+3", 5, 3, 10, 5},
    {"SQ 8+4", 3, 3, 8, 4},
    {"SQ 10+5", 3, 3, 10, 5},
    {"SQ 12+6", 3, 3, 12, 6},
    {"SQ 15+5", 3, 3, 15, 5},
};
static const int s_preset_count = sizeof(s_presets) / sizeof(s_presets[0]);

static int s_current_preset = 0;
static bool s_use_binfast = true;

// 画灰阶渐变条（16 级，每级用 4x4 Bayer 抖动模拟）
static void draw_grayscale_ramp(int x, int y, int bar_w, int bar_h) {
    int level_w = bar_w / 16;
    // 4x4 Bayer 抖动矩阵（归一化 0-15）
    static const int bayer[4][4] = {
        {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}
    };

    for (int i = 0; i < 16; i++) {
        int lx = x + i * level_w;
        // 灰阶 i：0=黑 15=白，阈值 = 15-i（值 < 阈值时画黑）
        int threshold = 15 - i;

        for (int py = 0; py < bar_h; py++) {
            for (int px = 0; px < level_w; px++) {
                int bx = px % 4, by = py % 4;
                int color = (bayer[by][bx] < threshold) ? EPD_GFX_BLACK : EPD_GFX_WHITE;
                epd_gfx_fill_rect(lx + px, y + py, 1, 1, color);
            }
        }
    }
    epd_gfx_draw_rect(x, y, bar_w, bar_h, EPD_GFX_BLACK);
}

// 画棋盘格（验证边界锐度）
static void draw_checkerboard(int x, int y, int w, int h, int cell_size) {
    int cols = w / cell_size;
    int rows = h / cell_size;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            uint16_t color = ((r + c) & 1) ? EPD_GFX_BLACK : EPD_GFX_WHITE;
            epd_gfx_fill_rect(x + c * cell_size, y + r * cell_size,
                             cell_size, cell_size, color);
        }
    }
    epd_gfx_draw_rect(x, y, cols * cell_size, rows * cell_size, EPD_GFX_BLACK);
}

// 画细线测试（1px/2px/3px/5px 水平+垂直线）
static void draw_fine_lines(int x, int y, int w, int h) {
    epd_gfx_fill_rect(x, y, w, h, EPD_GFX_WHITE);
    int line_y = y + 10;
    int line_x = x + 10;

    // 水平线
    epd_gfx_draw_hline(x, line_y, w, EPD_GFX_BLACK);      // 1px
    epd_gfx_fill_rect(x, line_y + 20, w, 2, EPD_GFX_BLACK); // 2px
    epd_gfx_fill_rect(x, line_y + 45, w, 3, EPD_GFX_BLACK); // 3px
    epd_gfx_fill_rect(x, line_y + 75, w, 5, EPD_GFX_BLACK); // 5px

    // 垂直线
    epd_gfx_draw_vline(line_x, y, h, EPD_GFX_BLACK);
    epd_gfx_fill_rect(line_x + 20, y, 2, h, EPD_GFX_BLACK);
    epd_gfx_fill_rect(line_x + 45, y, 3, h, EPD_GFX_BLACK);
    epd_gfx_fill_rect(line_x + 75, y, 5, h, EPD_GFX_BLACK);

    epd_gfx_draw_rect(x, y, w, h, EPD_GFX_BLACK);
}

// 画文本测试（多字号中英文）
static void draw_text_test(int x, int y, int w, int h) {
    epd_gfx_fill_rect(x, y, w, h, EPD_GFX_WHITE);

    int cy = y + 30;
    epd_gfx_draw_text(x + 20, cy, "Waveform Tuning Test", EPD_GFX_BLACK, 4);
    cy += 40;
    epd_gfx_draw_text(x + 20, cy, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", EPD_GFX_BLACK, 3);
    cy += 30;
    epd_gfx_draw_text(x + 20, cy, "abcdefghijklmnopqrstuvwxyz", EPD_GFX_BLACK, 3);
    cy += 30;
    epd_gfx_draw_text(x + 20, cy, "0123456789 !@#$%^&*()", EPD_GFX_BLACK, 3);
    cy += 40;
    epd_gfx_draw_text(x + 20, cy, "Fine text test 1234567890", EPD_GFX_BLACK, 2);
    cy += 25;
    epd_gfx_draw_text(x + 20, cy, "Smallest readable text", EPD_GFX_BLACK, 1);

    epd_gfx_draw_rect(x, y, w, h, EPD_GFX_BLACK);
}

// 画参数标签（醒目大字）
static void draw_param_label(int x, int y, const char* preset_name,
                             int n1, int n2, int nsat, int mmax, bool is_bf) {
    // 背景框
    epd_gfx_fill_rect(x, y, 500, 120, EPD_GFX_BLACK);

    // 参数文字（白底黑字）
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", preset_name);
    epd_gfx_draw_text(x + 20, y + 35, buf, EPD_GFX_WHITE, 5);

    if (is_bf) {
        snprintf(buf, sizeof(buf), "n1=%d n2=%d (%.2fs)", n1, n2, (n1 + n2) * 0.11);
    } else {
        snprintf(buf, sizeof(buf), "nsat=%d mmax=%d (%.2fs)", nsat, mmax, (nsat + mmax) * 0.11);
    }
    epd_gfx_draw_text(x + 20, y + 85, buf, EPD_GFX_WHITE, 3);
}

// 生成当前参数的完整测试图
static void generate_test_page(int preset_idx) {
    const TunePreset* p = &s_presets[preset_idx];

    // 应用参数
    EpdiyHighlevelState* hl = panel_es108fc_hl();
    if (s_use_binfast) {
        binfast_rebuild(p->bf_n1, p->bf_n2);
        epd_gfx_set_binfast(true);
        // binfast 波形由 epd_gfx_flush 内部临时替换
    } else {
        int map16[16];
        for (int t = 0; t < 16; t++) {
            map16[t] = (t * p->sq_mmax * 2 + 15) / 30;
        }
        scanq_rebuild(p->sq_nsat, p->sq_mmax, map16);
        epd_gfx_set_binfast(false);
        // scanq 波形需直接设置到 hl->waveform（epd_gfx 不自动替换）
        if (hl) {
            hl->waveform = scanq_waveform();
        }
    }

    // 清空画布
    epd_gfx_fill_screen(EPD_GFX_WHITE);

    // 布局：2x2 网格 + 顶部参数标签
    const int margin = 40;
    const int label_h = 140;
    const int cell_w = (W - 3 * margin) / 2;
    const int cell_h = (H - label_h - 3 * margin) / 2;

    // 参数标签（顶部居中）
    draw_param_label(margin, margin, p->name,
                    p->bf_n1, p->bf_n2, p->sq_nsat, p->sq_mmax, s_use_binfast);

    // 左上：灰阶渐变
    draw_grayscale_ramp(margin, label_h + margin, cell_w, cell_h);
    epd_gfx_draw_text(margin + 10, label_h + margin + 20, "Grayscale Ramp", EPD_GFX_BLACK, 2);

    // 右上：棋盘格
    draw_checkerboard(margin + cell_w + margin, label_h + margin, cell_w, cell_h, 40);
    epd_gfx_draw_text(margin + cell_w + margin + 10, label_h + margin + 20, "Checkerboard 40px", EPD_GFX_BLACK, 2);

    // 左下：细线测试
    draw_fine_lines(margin, label_h + cell_h + 2 * margin, cell_w, cell_h);
    epd_gfx_draw_text(margin + 10, label_h + cell_h + 2 * margin + 20, "Fine Lines 1/2/3/5px", EPD_GFX_BLACK, 2);

    // 右下：文本测试
    draw_text_test(margin + cell_w + margin, label_h + cell_h + 2 * margin, cell_w, cell_h);
    epd_gfx_draw_text(margin + cell_w + margin + 10, label_h + cell_h + 2 * margin + 20, "Text Test", EPD_GFX_BLACK, 2);

    // 底部提示
    char hint[128];
    snprintf(hint, sizeof(hint), "[%d/%d] n=next p=prev r=refresh i=info w=waveform",
             preset_idx + 1, s_preset_count);
    epd_gfx_draw_text(margin, H - 50, hint, EPD_GFX_BLACK, 2);
}

// 串口命令处理
static void process_command(char cmd) {
    switch (cmd) {
        case 'n':
        case 'N':
            s_current_preset = (s_current_preset + 1) % s_preset_count;
            ESP_LOGI(TAG, "下一组参数：%d/%d %s", s_current_preset + 1,
                    s_preset_count, s_presets[s_current_preset].name);
            generate_test_page(s_current_preset);
            epd_gfx_flush();
            break;

        case 'p':
        case 'P':
            s_current_preset = (s_current_preset - 1 + s_preset_count) % s_preset_count;
            ESP_LOGI(TAG, "上一组参数：%d/%d %s", s_current_preset + 1,
                    s_preset_count, s_presets[s_current_preset].name);
            generate_test_page(s_current_preset);
            epd_gfx_flush();
            break;

        case 'r':
        case 'R':
            ESP_LOGI(TAG, "重刷当前参数");
            epd_gfx_deep_clean();
            generate_test_page(s_current_preset);
            epd_gfx_flush();
            break;

        case 'i':
        case 'I': {
            const TunePreset* p = &s_presets[s_current_preset];
            ESP_LOGI(TAG, "=== 当前参数 ===");
            ESP_LOGI(TAG, "预设：%d/%d %s", s_current_preset + 1, s_preset_count, p->name);
            ESP_LOGI(TAG, "波形：%s", s_use_binfast ? "binfast" : "scanq");
            if (s_use_binfast) {
                ESP_LOGI(TAG, "binfast: n1=%d n2=%d (总 %d 扫 ≈ %.2fs)",
                        p->bf_n1, p->bf_n2, p->bf_n1 + p->bf_n2,
                        (p->bf_n1 + p->bf_n2) * 0.11);
            } else {
                ESP_LOGI(TAG, "scanq: nsat=%d mmax=%d (总 %d 扫 ≈ %.2fs)",
                        p->sq_nsat, p->sq_mmax, p->sq_nsat + p->sq_mmax,
                        (p->sq_nsat + p->sq_mmax) * 0.11);
            }
            break;
        }

        case 'w':
        case 'W':
            s_use_binfast = !s_use_binfast;
            ESP_LOGI(TAG, "切换波形：%s", s_use_binfast ? "binfast" : "scanq");
            generate_test_page(s_current_preset);
            epd_gfx_flush();
            break;

        case 'h':
        case 'H':
        case '?':
            ESP_LOGI(TAG, "命令：n=下一组 p=上一组 r=重刷 i=信息 w=波形 h=帮助");
            break;

        default:
            break;
    }
}

void tune_task(void* arg) {
    (void)arg;

    ESP_LOGI(TAG, "波形调参测试任务启动");
    ESP_LOGI(TAG, "串口命令：n=下一组 p=上一组 r=重刷 i=信息 w=波形 h=帮助");

    // 初始化绘图层
    if (epd_gfx_init() != 0) {
        ESP_LOGE(TAG, "绘图层初始化失败");
        vTaskDelete(NULL);
        return;
    }

    // 生成第一页测试图
    generate_test_page(s_current_preset);
    epd_gfx_flush();

    ESP_LOGI(TAG, "测试图已显示，等待串口命令...");

    // 主循环：等待串口命令
    while (1) {
        // 简单轮询串口（实际项目中应使用中断驱动）
        // 这里用延时模拟，实际命令处理在 main.c 的串口任务中
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// 外部命令接口（供串口任务调用）
void tune_process_command(char cmd) {
    process_command(cmd);
}
