/**
 * @file button_handler.c
 * @brief 五向导航按键扫描与去抖实现 (Task F-11)
 *
 * 策略：后台 FreeRTOS 任务，按 BUTTON_SCAN_MS 周期轮询；
 * 连续采样计数去抖，按下持续超过 LONG_PRESS_MS 触发长按，
 * 释放时若未达长按阈值则触发短按。
 */
#include "button_handler.h"
#include "debug_log.h"
#include "gpio_config.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "BUTTON";

static const int s_pins[NAV_KEY_COUNT] = {
    NAV_UP_PIN, NAV_DOWN_PIN, NAV_LEFT_PIN, NAV_RIGHT_PIN, NAV_CENTER_PIN,
    NAV_SET_PIN, NAV_RST_PIN
};

/* 日志用按键名 */
static const char *s_key_names[NAV_KEY_COUNT] = {
    "UP", "DOWN", "LEFT", "RIGHT", "CENTER", "SET", "RST"
};

/* 每个按键的运行时状态 */
typedef struct {
    bool     stable_pressed;   /* 去抖后电平：true=按下 */
    bool     long_fired;       /* 本次按下是否已触发长按 */
    uint16_t debounce_cnt;     /* 去抖计数 */
    uint32_t press_tick;       /* 按下起始时刻(ms) */
} btn_state_t;

static btn_state_t s_state[NAV_KEY_COUNT];
static button_callback_t s_callback = NULL;

/* 后台扫描任务（在 button_handler_init 中创建） */
void button_scan_task(void *arg);

static uint32_t millis(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

int button_handler_init(void)
{
    /* 配置五向开关为上拉输入 */
    uint64_t mask = 0;
    for (int i = 0; i < NAV_KEY_COUNT; i++) {
        mask |= (1ULL << s_pins[i]);
    }
    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    memset(s_state, 0, sizeof(s_state));

    /* 启动扫描任务 */
    xTaskCreate(button_scan_task, "btn_scan", 4096, NULL, 5, NULL);
    LOG_I("nav switch handler started (UP=%d DOWN=%d LEFT=%d RIGHT=%d CENTER=%d SET=%d RST=%d)",
          NAV_UP_PIN, NAV_DOWN_PIN, NAV_LEFT_PIN, NAV_RIGHT_PIN,
          NAV_CENTER_PIN, NAV_SET_PIN, NAV_RST_PIN);
    return 0;
}

void button_register_callback(button_callback_t cb)
{
    s_callback = cb;
}

bool button_is_pressed(nav_key_t id)
{
    if (id < 0 || id >= NAV_KEY_COUNT) return false;
    return s_state[id].stable_pressed;
}

void button_scan_task(void *arg)
{
    (void)arg;
    const uint16_t debounce_ticks = BUTTON_DEBOUNCE_MS / BUTTON_SCAN_MS;
    const uint32_t long_threshold = BUTTON_LONG_PRESS_MS;

    while (1) {
        for (int i = 0; i < NAV_KEY_COUNT; i++) {
            /* 低电平有效（上拉，COM 接地） */
            bool raw = (gpio_get_level(s_pins[i]) == 0);

            btn_state_t *st = &s_state[i];

            /* ---- 去抖 ---- */
            if (raw == st->stable_pressed) {
                st->debounce_cnt = 0;
            } else {
                st->debounce_cnt++;
                if (st->debounce_cnt >= debounce_ticks) {
                    st->stable_pressed = raw;
                    st->debounce_cnt = 0;
                    if (raw) {
                        /* 刚刚稳定为“按下” */
                        st->press_tick = millis();
                        st->long_fired = false;
                    }
                }
            }

            /* ---- 长按判定（按住中） ---- */
            if (st->stable_pressed && !st->long_fired) {
                if ((millis() - st->press_tick) >= long_threshold) {
                    st->long_fired = true;
                    if (s_callback) s_callback((nav_key_t)i, BUTTON_EVENT_LONG_PRESS);
                    LOG_D("NAV_%s LONG", s_key_names[i]);
                }
            }

            /* ---- 短按判定（释放时） ---- */
            if (!st->stable_pressed) {
                if (st->press_tick != 0 && !st->long_fired) {
                    /* 释放且未曾触发长按 -> 短按 */
                    if (s_callback) s_callback((nav_key_t)i, BUTTON_EVENT_SHORT_PRESS);
                    LOG_D("NAV_%s SHORT", s_key_names[i]);
                }
                st->press_tick = 0;
                st->long_fired = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_SCAN_MS));
    }
}
