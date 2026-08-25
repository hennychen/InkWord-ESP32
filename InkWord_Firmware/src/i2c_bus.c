/**
 * @file i2c_bus.c
 * @brief 公共 I2C 总线实现（v1.2 T2.6，见 i2c_bus.h 边界约定）
 *
 * 命令链构造与 es8311.c 原内嵌实现同源（esp-adf 移植脉络），收口后
 * 加互斥锁；锁创建经 portMUX 临界区防双任务首载竞态（创建本身在
 * 临界区外——alloc 不可入临界区）。
 */
#include "i2c_bus.h"
#include "gpio_config.h"   /* 总线参数宏（现名 ES8311_I2C_*） */

#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include <stdbool.h>

static const char *TAG = "I2CBUS";

static SemaphoreHandle_t s_lock = NULL;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_inited = false;

/* 总线占用：xSemaphoreTake 给出 pdMS_TO_TICKS(100)（与单命令超时
 * 同量级，两设备交替短事务不会长时间互卡） */
#define I2CBUS_LOCK_MS 100

int i2c_bus_init(void)
{
    portENTER_CRITICAL(&s_init_mux);
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    portEXIT_CRITICAL(&s_init_mux);
    if (!s_lock) {
        ESP_LOGE(TAG, "mutex create failed");
        return -1;
    }

    if (!s_inited) {
        i2c_config_t cfg = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = ES8311_I2C_SDA_PIN,
            .scl_io_num = ES8311_I2C_SCL_PIN,
            .sda_pullup_en = GPIO_PULLUP_ENABLE,
            .scl_pullup_en = GPIO_PULLUP_ENABLE,
            .master.clk_speed = ES8311_I2C_FREQ_HZ,
        };
        if (i2c_param_config(ES8311_I2C_NUM, &cfg) != ESP_OK ||
            i2c_driver_install(ES8311_I2C_NUM, I2C_MODE_MASTER, 0, 0, 0)
                != ESP_OK) {
            ESP_LOGE(TAG, "bus init failed (sda=%d scl=%d)",
                     ES8311_I2C_SDA_PIN, ES8311_I2C_SCL_PIN);
            return -1;
        }
        s_inited = true;
        ESP_LOGI(TAG, "bus ready (num=%d sda=%d scl=%d)",
                 ES8311_I2C_NUM, ES8311_I2C_SDA_PIN, ES8311_I2C_SCL_PIN);
    }
    return 0;
}

static bool bus_take(void)
{
    return s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(I2CBUS_LOCK_MS))
                       == pdTRUE;
}

static void bus_give(void)
{
    xSemaphoreGive(s_lock);
}

int i2c_bus_write_reg(uint8_t addr, uint8_t reg, uint8_t val)
{
    if (!s_inited || !bus_take()) return -1;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (uint8_t)(addr << 1), 1);
    i2c_master_write_byte(cmd, reg, 1);
    i2c_master_write_byte(cmd, val, 1);
    i2c_master_stop(cmd);
    esp_err_t r = i2c_master_cmd_begin(ES8311_I2C_NUM, cmd,
                                       pdMS_TO_TICKS(I2CBUS_LOCK_MS));
    i2c_cmd_link_delete(cmd);
    bus_give();
    return (r == ESP_OK) ? 0 : -1;
}

int i2c_bus_read_reg(uint8_t addr, uint8_t reg, uint8_t *out)
{
    if (!s_inited || !bus_take()) return -1;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (uint8_t)(addr << 1), 1);
    i2c_master_write_byte(cmd, reg, 1);
    i2c_master_stop(cmd);
    esp_err_t r = i2c_master_cmd_begin(ES8311_I2C_NUM, cmd,
                                       pdMS_TO_TICKS(I2CBUS_LOCK_MS));
    i2c_cmd_link_delete(cmd);
    if (r != ESP_OK) {
        bus_give();
        return -1;
    }

    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (uint8_t)((addr << 1) | 1), 1);
    i2c_master_read_byte(cmd, out, 0x01 /*NACK*/);
    i2c_master_stop(cmd);
    r = i2c_master_cmd_begin(ES8311_I2C_NUM, cmd,
                             pdMS_TO_TICKS(I2CBUS_LOCK_MS));
    i2c_cmd_link_delete(cmd);
    bus_give();
    return (r == ESP_OK) ? 0 : -1;
}

int i2c_bus_read_reg16(uint8_t addr, uint8_t reg, uint16_t *out)
{
    if (!s_inited || !bus_take()) return -1;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (uint8_t)(addr << 1), 1);
    i2c_master_write_byte(cmd, reg, 1);
    i2c_master_stop(cmd);
    esp_err_t r = i2c_master_cmd_begin(ES8311_I2C_NUM, cmd,
                                       pdMS_TO_TICKS(I2CBUS_LOCK_MS));
    i2c_cmd_link_delete(cmd);
    if (r != ESP_OK) {
        bus_give();
        return -1;
    }

    uint8_t b[2];
    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (uint8_t)((addr << 1) | 1), 1);
    i2c_master_read_byte(cmd, &b[0], 0x0 /*ACK*/);
    i2c_master_read_byte(cmd, &b[1], 0x01 /*NACK*/);
    i2c_master_stop(cmd);
    r = i2c_master_cmd_begin(ES8311_I2C_NUM, cmd,
                             pdMS_TO_TICKS(I2CBUS_LOCK_MS));
    i2c_cmd_link_delete(cmd);
    bus_give();
    if (r != ESP_OK) return -1;
    *out = (uint16_t)((b[0] << 8) | b[1]);   /* 大端（MAX17048 惯例） */
    return 0;
}
