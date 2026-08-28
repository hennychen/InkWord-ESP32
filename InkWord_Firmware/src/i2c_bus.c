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
#include "driver/gpio.h"       /* 总线恢复：开漏接管引脚（2026-08-28） */
#include "rom/ets_sys.h"       /* ets_delay_us：SCL 脉冲微秒级节拍 */
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

/* 驱动参数与装载（init/recover 共用，单一真相源防双份漂移） */
static bool bus_take(void);      /* recover 先于定义使用，前置声明 */
static void bus_give(void);

static int bus_install_driver(void)
{
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = ES8311_I2C_SDA_PIN,
        .scl_io_num = ES8311_I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = ES8311_I2C_FREQ_HZ,
    };
    if (i2c_param_config(ES8311_I2C_NUM, &cfg) != ESP_OK ||
        i2c_driver_install(ES8311_I2C_NUM, I2C_MODE_MASTER, 0, 0, 0) != ESP_OK)
        return -1;
    return 0;
}

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
        if (bus_install_driver() != 0) {
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

/* ---- 总线死锁恢复（2026-08-28） ----
 * 从机在事务中途失宿主（MCU 复位/串口毛刺等）会卡在“等第 9 个
 * clock”状态持续拉低 SDA，此后整条总线 NACK/超时。标准恢复：
 * 卸驱动接管引脚 → 9×SCL 脉冲喂完挂起事务 → STOP → 重装驱动 */
int i2c_bus_recover(void)
{
    if (!s_inited) return -1;
    if (!bus_take()) return -1;

    /* 1. 卸载驱动，开漏接管 SCL/SDA（模拟 I2C 物理层时序） */
    i2c_driver_delete(ES8311_I2C_NUM);
    gpio_config_t g = {
        .pin_bit_mask = (1ULL << ES8311_I2C_SDA_PIN) |
                        (1ULL << ES8311_I2C_SCL_PIN),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&g);

    /* 2. 空闲电平诊断（健康 1/1；SDA=0 即死锁实锤，日志留证） */
    ESP_LOGW(TAG, "recover: idle sda=%d scl=%d",
             gpio_get_level(ES8311_I2C_SDA_PIN),
             gpio_get_level(ES8311_I2C_SCL_PIN));

    /* 3. SCL 9 脉冲：从机补完挂起事务即释放 SDA */
    gpio_set_level(ES8311_I2C_SCL_PIN, 1);
    for (int i = 0; i < 9; i++) {
        gpio_set_level(ES8311_I2C_SCL_PIN, 0);
        ets_delay_us(5);
        gpio_set_level(ES8311_I2C_SCL_PIN, 1);
        ets_delay_us(5);
        if (gpio_get_level(ES8311_I2C_SDA_PIN))
            break;                    /* SDA 已释放，余下时钟免打 */
    }

    /* 3.5 地址事务重同步（2026-08-28 实测补充）：总线电平健康
     * （sda=1/scl=1）但器件持续 NACK 时，地址检测器可能卡在非法
     * 态——标准 9×SCL+STOP 不含 START，地址匹配逻辑无法重新同步。
     * 位脉冲手发一个完整寻址事务：START + 0x18<<1|W + ACK 槽 + STOP */
    gpio_set_level(ES8311_I2C_SDA_PIN, 1);
    ets_delay_us(5);
    gpio_set_level(ES8311_I2C_SDA_PIN, 0);   /* START：SCL 高时 SDA 降 */
    ets_delay_us(5);
    for (int bit = 7; bit >= 0; bit--) {     /* 0x30 = 0x18<<1 | W */
        gpio_set_level(ES8311_I2C_SCL_PIN, 0);
        ets_delay_us(5);
        gpio_set_level(ES8311_I2C_SDA_PIN,
                       (0x30 >> bit) & 1);
        ets_delay_us(5);
        gpio_set_level(ES8311_I2C_SCL_PIN, 1);
        ets_delay_us(5);
    }
    gpio_set_level(ES8311_I2C_SCL_PIN, 0);   /* ACK 槽（释放 SDA 任其浮） */
    ets_delay_us(5);
    gpio_set_level(ES8311_I2C_SCL_PIN, 1);
    ets_delay_us(5);

    /* 4. STOP：SCL 高期间 SDA 低→高 */
    gpio_set_level(ES8311_I2C_SDA_PIN, 0);
    ets_delay_us(5);
    gpio_set_level(ES8311_I2C_SDA_PIN, 1);
    ets_delay_us(5);

    /* 5. 重装驱动（与 init 同参，单一真相源） */
    if (bus_install_driver() != 0) {
        ESP_LOGE(TAG, "recover: driver reinstall failed");
        s_inited = false;
        bus_give();
        return -1;
    }
    bus_give();
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
