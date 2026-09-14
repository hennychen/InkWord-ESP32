// ES108FC1C1-RHY 面板安全初始化（方案 §3.3）
// 安全链：PSRAM 容量检查 → epd_init（V7 板 + 保守总线速率）→ VCOM
// 范围检查（1000~2500mV）→ 全白清除验证基本功能。
#include "driver/panel_es108fc.h"

#include <stdbool.h>
#include <stddef.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "epdiy.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config/display_config.h"
#include "driver/board_config.h"

static const char* TAG = "panel";

// 显示描述（卖家示例对齐：波形 ED047TC1 起步，display_config.h 勘误记录）
// 非 const：bring-up 步骤 8 速率爬升需运行时改 bus_speed 后重 init
static EpdDisplay_t s_display = {
    .width = PANEL_WIDTH,
    .height = PANEL_HEIGHT,
    .bus_width = PANEL_BUS_WIDTH,
    .bus_speed = PANEL_BUS_SPEED_MHZ,
    .default_waveform = &PANEL_WAVEFORM,
    .display_type = DISPLAY_TYPE_GENERIC,
};

static EpdiyHighlevelState s_hl;
static bool s_epd_ready = false;
static bool s_hl_ready = false;
static bool s_pmic_online = false;

// I2C 驱动幂等安装：卖家魔改版未装库内 I2C，此处补装（I2C_NUM_0，
// 39/40），供库残留的 tps_read_register 轮询与温度读取使用；
// 重复安装返回 INVALID_STATE 属正常，忽略。
static void ensure_i2c_driver(void) {
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
        return;
    }
    err = i2c_driver_install(BS_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "I2C 驱动安装失败: %s", esp_err_to_name(err));
    }
}

void panel_es108fc_set_pmic_online(bool online) {
    s_pmic_online = online;
    ESP_LOGW(TAG, "电源路径钉定: %s", online ? "库 PMIC 链（真 PG 等待）"
                                              : "GPIO46 直控（卖家板型）");
}

void panel_power_on(void) {
    // GPIO46 双保险：PMIC 在线时同步拉高无害，缺失时为唯一电源路径
    gpio_set_level(BS_EPD_POWER_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));  // 电源稳定窗
    if (s_pmic_online) {
        epd_poweron();  // 库路径（卖家版残留 tps PG 轮询在 I2C 就绪后生效）
    }
}

void panel_power_off(void) {
    if (s_pmic_online) {
        epd_poweroff();
    }
    gpio_set_level(BS_EPD_POWER_EN, 0);
}

esp_err_t panel_es108fc_safe_init(void) {
    // 1. PSRAM 可用性检查（帧缓冲依赖）
    //    方案原文用 assert，release 构建（NDEBUG）下会被编译剔除，
    //    故改为运行时检查 + 错误返回（安全检查不可被优化掉）
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram_total < 2 * 1024 * 1024) {
        ESP_LOGE(TAG, "PSRAM 不足 2MB（实测 %zu 字节），帧缓冲无法保障", psram_total);
        return ESP_ERR_NO_MEM;
    }

    // 幂等重入：速率爬升场景先 deinit 旧实例（hl 帧缓冲复用，见下）
    if (s_epd_ready) {
        epd_deinit();
        s_epd_ready = false;
    }

    // I2C 先行：卖家魔改版不装库内 I2C，此处补装后再 init 库
    ensure_i2c_driver();

    // GPIO46 电源使能先断电复位（对齐卖家 .ino：setup 先拉低）
    gpio_reset_pin(BS_EPD_POWER_EN);
    gpio_set_direction(BS_EPD_POWER_EN, GPIO_MODE_OUTPUT);
    gpio_set_level(BS_EPD_POWER_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(20));

    // 2. epdiy 初始化（保守参数：bus_speed 由 display_config.h 控制）
    epd_init(&epd_board_v7, &s_display, EPD_LUT_64K);
    s_epd_ready = true;

    // 3. VCOM 范围检查（越界拒绝上电——防烧屏红线，方案 §3.3）
    //    卖家魔改版 set_vcom 为空壳（只存库内变量不落硬件），实际 VCOM
    //    由板载电位器决定；此处保留检查与调用以对齐官方 API 语义并
    //    记录目标值，真机校准时以屏显实测为准（遗留 TODO-B）。
    if (PANEL_VCOM_MV < 1000 || PANEL_VCOM_MV > 2500) {
        ESP_LOGE(TAG, "VCOM 超出安全范围: %d mV（允许 1000~2500）", PANEL_VCOM_MV);
        epd_deinit();
        s_epd_ready = false;
        return ESP_ERR_INVALID_ARG;
    }
    epd_set_vcom(PANEL_VCOM_MV);

    // 高层状态只分配一次：epd_deinit 不释放 hl 帧缓冲，重复
    // epd_hl_init 会重新分配 ~1MB PSRAM，速率爬升多轮会耗尽
    if (!s_hl_ready) {
        s_hl = epd_hl_init(EPD_BUILTIN_WAVEFORM);
        s_hl_ready = true;
    }

    // 4. 全白清除（验证上电/电源链/波形基本链路，双路径电源）
    panel_power_on();
    epd_clear();
    panel_power_off();

    ESP_LOGI(TAG, "ES108FC1C1-RHY 初始化完成（bus=%dMHz, VCOM 目标=%dmV, 波形=ED047TC1 起步）",
             (int)s_display.bus_speed, PANEL_VCOM_MV);
    return ESP_OK;
}

EpdiyHighlevelState* panel_es108fc_hl(void) {
    return s_hl_ready ? &s_hl : NULL;
}

EpdDisplay_t* panel_es108fc_display(void) {
    return &s_display;
}

void panel_es108fc_deinit(void) {
    panel_power_off();
    if (s_epd_ready) {
        epd_deinit();
        s_epd_ready = false;
    }
}
