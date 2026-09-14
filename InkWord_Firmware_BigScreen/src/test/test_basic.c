// 八步安全 bring-up 测试序列（方案 §3.4）
// 风险从低到高分步验证：PSRAM → I2C → 上电 → 清屏 → 像素 → 灰阶 →
// 文字 → 速率爬升。每步之间固定观察窗供人工确认屏幕状态，
// 任何异常立即断电（观察窗内日志会反复提醒）。
//
// 仅在 BIGSCREEN_PROBE（bigscreen-probe env）下由 main.c 调用完整序列。
#include <stdio.h>
#include <string.h>

#include "driver/i2c.h"
#include "driver/panel_es108fc.h"
#include "epdiy.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config/display_config.h"
#include "driver/board_config.h"
#include "fonts/firasans_12.h"
#include "fonts/firasans_20.h"
#include "test_basic.h"

#define STEP_DELAY_MS 8000  // 步间人工确认观察窗（方案：每步人工确认）

static const char* TAG = "bringup";

static void step_banner(int step, const char* msg) {
    ESP_LOGW(TAG, "==== 步骤 %d/8: %s ====", step, msg);
    ESP_LOGW(TAG, "目视确认屏幕状态，%d 秒后继续；异常请立即断电！", STEP_DELAY_MS / 1000);
}

static void step_wait(void) {
    for (int i = STEP_DELAY_MS / 1000; i > 0; i--) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ---- 步骤 1：PSRAM 检测（帧缓冲依赖，>= 2MB） ----
static bool step1_psram(void) {
    size_t total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t free_ = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PSRAM total=%zu free=%zu（要求 >= 2MB）", total, free_);
    if (total < 2 * 1024 * 1024) {
        ESP_LOGE(TAG, "PSRAM 不足 2MB，中止后续所有步骤");
        return false;
    }
    return true;
}

// ---- 步骤 2：I2C 扫描（TPS65185=0x48 / PCA9555=0x20 必须应答） ----
// 在 epd_init 之前裸扫：先装临时驱动 → 扫描 → 卸载，交还 epdiy 库接管
static bool step2_i2c_scan(void) {
    i2c_config_t conf;
    memset(&conf, 0, sizeof(conf));
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = BS_I2C_SDA;
    conf.scl_io_num = BS_I2C_SCL;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = 100000;

    esp_err_t err = i2c_param_config(BS_I2C_PORT, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C 参数配置失败: %s", esp_err_to_name(err));
        return false;
    }
    err = i2c_driver_install(BS_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C 驱动安装失败: %s", esp_err_to_name(err));
        return false;
    }

    const uint8_t expect_tps = 0x48;  // TPS65185 PMIC
    const uint8_t expect_pca = 0x20;  // PCA9555 IO 扩展
    bool tps_found = false;
    bool pca_found = false;

    ESP_LOGI(TAG, "I2C 总线扫描（SDA=%d SCL=%d）...", BS_I2C_SDA, BS_I2C_SCL);
    for (uint8_t addr = 1; addr < 0x7F; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (uint8_t)(addr << 1) | I2C_MASTER_READ, true);
        i2c_master_stop(cmd);
        esp_err_t r = i2c_master_cmd_begin(BS_I2C_PORT, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
        if (r == ESP_OK) {
            ESP_LOGI(TAG, "  I2C 设备应答: 0x%02X", addr);
            if (addr == expect_tps) tps_found = true;
            if (addr == expect_pca) pca_found = true;
        }
    }
    // 驱动保留不删：后续库残留 tps_read/温度读取复用
    // （卖家魔改版不装库内 I2C，panel 会幂等补装）

    // 板型判定钉电源路径（卖家板可能无 PMIC，改走 GPIO46 直控）：
    bool pmic_online = tps_found && pca_found;
    panel_es108fc_set_pmic_online(pmic_online);
    if (!pmic_online) {
        ESP_LOGW(TAG, "TPS65185/PCA9555 未全应答（tps=%d pca=%d）——判定为"
                  " GPIO46 直控电源型卖家板，继续 bring-up",
                 tps_found, pca_found);
    }
    return true;
}

// ---- 步骤 3：上电序列（双路径电源 + PG 链路探测） ----
// PMIC 型：panel_power_on 内库 epd_poweron 走真 PG 等待；
// GPIO46 直控型：使能脚拉高 + 稳定窗。温度经 I2C 读 TPS 寄存器，
// 直控型无 PMIC 时读数无意义（仅参考）。
static bool step3_poweron(void) {
    panel_power_on();
    int temperature = epd_ambient_temperature();
    panel_power_off();
    ESP_LOGI(TAG, "上电/下电循环 OK（双路径自动）；温度读数 %d（仅 PMIC 型有意义）",
             temperature);
    return true;
}

// ---- 步骤 4：全白清除（验证波形基本功能） ----
static bool step4_clear(void) {
    panel_power_on();
    epd_clear();
    panel_power_off();
    ESP_LOGI(TAG, "全白清除完成——屏幕应为纯白，残留灰影/花屏即波形异常");
    return true;
}

// ---- 步骤 5：四角像素（验证坐标映射与走线完整性） ----
static bool step5_corner_pixels(void) {
    EpdiyHighlevelState* hl = panel_es108fc_hl();
    uint8_t* fb = epd_hl_get_framebuffer(hl);
    epd_hl_set_all_white(hl);

    const int m = 30;  // 角点内边距
    const int r = 12;  // 圆点半径
    epd_fill_circle(m, m, r, 0x0, fb);                              // 左上
    epd_fill_circle(PANEL_WIDTH - m, m, r, 0x0, fb);                // 右上
    epd_fill_circle(m, PANEL_HEIGHT - m, r, 0x0, fb);               // 左下
    epd_fill_circle(PANEL_WIDTH - m, PANEL_HEIGHT - m, r, 0x0, fb); // 右下
    epd_draw_pixel(PANEL_WIDTH / 2, PANEL_HEIGHT / 2, 0x0, fb);     // 中心单像素

    panel_power_on();
    enum EpdDrawError err =
        epd_hl_update_screen(hl, MODE_GC16, epd_ambient_temperature());
    panel_power_off();
    if (err != EPD_DRAW_SUCCESS) {
        ESP_LOGE(TAG, "像素帧刷新错误: 0x%X", (unsigned int)err);
        return false;
    }
    ESP_LOGI(TAG, "四角黑点 + 中心单像素——四点齐且无偏移/镜像为通过");
    return true;
}

// ---- 步骤 6：16 级灰阶渐变（验证波形灰度质量） ----
static bool step6_grayscale(void) {
    EpdiyHighlevelState* hl = panel_es108fc_hl();
    uint8_t* fb = epd_hl_get_framebuffer(hl);
    epd_hl_set_all_white(hl);

    const int bar_w = PANEL_WIDTH / 16;
    const int bar_y = PANEL_HEIGHT / 4;
    const int bar_h = PANEL_HEIGHT / 2;
    for (int i = 0; i < 16; i++) {
        EpdRect bar = {
            .x = i * bar_w,
            .y = bar_y,
            .width = bar_w,
            .height = bar_h,
        };
        epd_fill_rect(bar, (uint8_t)i, fb);  // 4bpp：0=黑 ... 15=白
    }

    panel_power_on();
    enum EpdDrawError err =
        epd_hl_update_screen(hl, MODE_GC16, epd_ambient_temperature());
    panel_power_off();
    if (err != EPD_DRAW_SUCCESS) {
        ESP_LOGE(TAG, "灰阶帧刷新错误: 0x%X", (unsigned int)err);
        return false;
    }
    ESP_LOGI(TAG, "16 级灰阶条带（左黑右白）——相邻级可辨、无跳变/残留为通过");
    return true;
}

// ---- 步骤 7：水平时序诊断帧（实测：文字碎裂左右下角+圆圈边缘锯齿） ----
// 图案：64px 黑白棋盘格全屏 + 顶部 16px 高水平标尺带（8px 周期条纹）。
// 判读：
//   ① 棋盘错缝/锯齿墙 → 行内像素错位（DE 窗口/back porch 不适配 1920 宽屏）
//   ② 棋盘行错位累计 → 垂直拉伸/批推进错拍
//   ③ 标尺带条纹密度突变行 → DMA 供给断流起点
static bool step7_text(void) {
    EpdiyHighlevelState* hl = panel_es108fc_hl();
    uint8_t* fb = epd_hl_get_framebuffer(hl);
    epd_hl_set_all_white(hl);

    // 64px 黑白棋盘格全屏
    const int cell = 64;
    for (int by = 0; by + cell <= PANEL_HEIGHT; by += cell) {
        for (int bx = 0; bx + cell <= PANEL_WIDTH; bx += cell) {
            if (((bx / cell) + (by / cell)) % 2 == 0) {
                EpdRect r = {bx, by, cell, cell};
                epd_fill_rect(r, 0x0, fb);
            }
        }
    }

    // 顶部标尺带：8px 周期条纹（白底，覆盖棋盘顶部 16px 避免透出干扰判读）
    EpdRect ruler_bg = {0, 0, PANEL_WIDTH, 16};
    epd_fill_rect(ruler_bg, 0xFF, fb);
    for (int x = 0; x + 8 <= PANEL_WIDTH; x += 16) {
        EpdRect r = {x, 0, 8, 16};
        epd_fill_rect(r, 0x0, fb);
    }

    panel_power_on();
    enum EpdDrawError err =
        epd_hl_update_screen(hl, MODE_GC16, epd_ambient_temperature());
    panel_power_off();
    if (err != EPD_DRAW_SUCCESS) {
        ESP_LOGE(TAG, "时序诊断帧刷新错误: 0x%X", (unsigned int)err);
        return false;
    }
    ESP_LOGI(TAG, "时序帧：64px 棋盘 + 顶部 8px 周期标尺带");
    return true;
}

// ---- 步骤 8：总线速率爬升（10MHz 起步已验证，逐级 12→15→17MHz） ----
// 每档：deinit → 改 bus_speed → 重 init（内部含全白清除）→ 画验证帧
static bool step8_bus_speed_climb(void) {
    static const int speeds[] = {12, 15, 17};
    const int n = (int)(sizeof(speeds) / sizeof(speeds[0]));

    for (int i = 0; i < n; i++) {
        EpdDisplay_t* disp = panel_es108fc_display();
        disp->bus_speed = (uint8_t)speeds[i];
        ESP_LOGW(TAG, "速率爬升 %d/%d：切换到 %d MHz ...", i + 1, n, speeds[i]);

        // 幂等重入：内部 deinit → epd_init → VCOM → 全白清除
        if (panel_es108fc_safe_init() != ESP_OK) {
            ESP_LOGE(TAG, "%d MHz 档初始化失败，保持 10MHz 结论", speeds[i]);
            return false;
        }

        // 每档画四角点 + 灰阶条复合帧，快速暴露高速下的走线错误
        EpdiyHighlevelState* hl = panel_es108fc_hl();
        uint8_t* fb = epd_hl_get_framebuffer(hl);
        epd_hl_set_all_white(hl);
        epd_fill_circle(30, 30, 12, 0x0, fb);
        epd_fill_circle(PANEL_WIDTH - 30, 30, 12, 0x0, fb);
        epd_fill_circle(30, PANEL_HEIGHT - 30, 12, 0x0, fb);
        epd_fill_circle(PANEL_WIDTH - 30, PANEL_HEIGHT - 30, 12, 0x0, fb);
        const int bar_w = PANEL_WIDTH / 16;
        for (int k = 0; k < 16; k++) {
            EpdRect bar = {
                .x = k * bar_w,
                .y = PANEL_HEIGHT / 4,
                .width = bar_w,
                .height = PANEL_HEIGHT / 2,
            };
            epd_fill_rect(bar, (uint8_t)k, fb);
        }
        panel_power_on();
        enum EpdDrawError err =
            epd_hl_update_screen(hl, MODE_GC16, epd_ambient_temperature());
        panel_power_off();
        if (err != EPD_DRAW_SUCCESS) {
            ESP_LOGE(TAG, "%d MHz 档刷新错误 0x%X——回落上一稳定档",
                     speeds[i], (unsigned int)err);
            return false;
        }
        ESP_LOGW(TAG, "%d MHz 档已渲染，请在观察窗内确认无花屏/条纹", speeds[i]);
        step_wait();
    }

    // 收尾：恢复保守默认档，便于后续开发从安全值起步
    panel_es108fc_display()->bus_speed = PANEL_BUS_SPEED_MHZ;
    panel_es108fc_safe_init();
    ESP_LOGI(TAG, "速率爬升完成，已回落默认 %d MHz", PANEL_BUS_SPEED_MHZ);
    return true;
}

int bringup_run_all(void) {
    // 步骤 1~2 无屏介入，先验证主板基础外设
    step_banner(1, "PSRAM 检测");
    if (!step1_psram()) return 1;
    step_banner(2, "I2C 扫描（TPS65185/PCA9555）");
    if (!step2_i2c_scan()) return 2;

    // 面板安全初始化（epd_init + VCOM 范围检查 + 全白清除，方案 §3.3）
    esp_err_t err = panel_es108fc_safe_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "面板安全初始化失败: %s", esp_err_to_name(err));
        return 3;
    }

    step_banner(3, "上电序列（PMIC PG 链路）");
    if (!step3_poweron()) return 3;
    step_wait();
    step_banner(4, "全白清除");
    if (!step4_clear()) return 4;
    step_wait();
    step_banner(5, "四角像素 + 中心点");
    if (!step5_corner_pixels()) return 5;
    step_wait();
    step_banner(6, "16 级灰阶渐变");
    if (!step6_grayscale()) return 6;
    step_wait();
    step_banner(7, "文字渲染");
    if (!step7_text()) return 7;
    step_wait();
    step_banner(8, "总线速率爬升 12→15→17MHz");
    if (!step8_bus_speed_climb()) return 8;

    ESP_LOGW(TAG, "==== 八步序列全部通过 ====");
    return 0;
}

// [InkWord 修复] bring-up 独立高优先级任务包装：主控必须与 epdiy feed
// 线程同优先级，否则渲染忙等轮转永远轮不到低优先级主控（静默挂死）。
void bringup_run_all_task(void *arg) {
    int rc = bringup_run_all();
    ESP_LOGW(TAG, "bring-up 序列退出码 %d（0=全部通过）", rc);
    vTaskDelete(NULL);
}
