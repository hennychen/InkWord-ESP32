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
#include "haptic.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"  /* T0.2：事件队列（扫描任务 → 主任务） */

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
static button_callback_t s_callback = NULL;  /* 遗留声明（T0.2 后不再触发） */

/* 事件队列（T0.2，修 C2）：扫描任务生产，主任务 loop 经 button_wait
 * 消费。深度 16 覆盖刷新阻塞窗口（三色屏全刷 14.6s 内的连按）；
 * 满则丢最旧保最新（丢中间事件会破坏短按/长按配对语义，丢最旧
 * 至少保住最近的用户意图） */
#define BUTTON_QUEUE_DEPTH 16
static QueueHandle_t s_event_queue = NULL;

typedef struct {
    nav_key_t      id;
    button_event_t ev;
} button_msg_t;

/* 入队（扫描任务上下文）：满则丢最旧再入，告警留进按计数证据 */
static void button_enqueue(nav_key_t id, button_event_t ev)
{
    button_msg_t msg = { id, ev };
    if (xQueueSend(s_event_queue, &msg, 0) != pdTRUE) {
        button_msg_t dropped;
        xQueueReceive(s_event_queue, &dropped, 0);
        xQueueSend(s_event_queue, &msg, 0);
        LOG_W("event queue full, dropped NAV_%s %s",
              s_key_names[(int)dropped.id],
              dropped.ev == BUTTON_EVENT_LONG_PRESS ? "LONG" : "SHORT");
    }
}

bool button_wait(nav_key_t *id, button_event_t *ev, uint32_t timeout_ms)
{
    if (!s_event_queue || !id || !ev) return false;
    button_msg_t msg;
    if (xQueueReceive(s_event_queue, &msg, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
        return false;
    *id = msg.id;
    *ev = msg.ev;
    return true;
}

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

    /* T0.2：事件队列先于扫描任务创建（扫描任务首个事件即依赖） */
    if (!s_event_queue)
        s_event_queue = xQueueCreate(BUTTON_QUEUE_DEPTH, sizeof(button_msg_t));

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
                        /* 刚刚稳定为“按下”：全局 20ms 短震（PRD 5.4，
                         * 覆盖配网/待机等全部页面；语义档反馈由 main.cpp 补位） */
                        st->press_tick = millis();
                        st->long_fired = false;
                        haptic_event(HAPTIC_KEYPRESS);
                    }
                }
            }

            /* ---- 长按判定（按住中） ---- */
            if (st->stable_pressed && !st->long_fired) {
                if ((millis() - st->press_tick) >= long_threshold) {
                    st->long_fired = true;
                    /* T0.2：入队交主任务处理（扫描任务不再直接回调，
                     * 避免重活在扫描上下文阻塞去抖/长按采样） */
                    button_enqueue((nav_key_t)i, BUTTON_EVENT_LONG_PRESS);
                    /* INFO 级：串口默认 INFO（log_init），按键事件是接线
                     * 验证的关键证据链，不用 DEBUG 级 */
                    LOG_I("NAV_%s LONG", s_key_names[i]);
                }
            }

            /* ---- 短按判定（释放时） ---- */
            if (!st->stable_pressed) {
                if (st->press_tick != 0 && !st->long_fired) {
                    /* 释放且未曾触发长按 -> 短按（T0.2 入队） */
                    button_enqueue((nav_key_t)i, BUTTON_EVENT_SHORT_PRESS);
                    LOG_I("NAV_%s SHORT", s_key_names[i]);
                }
                st->press_tick = 0;
                st->long_fired = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_SCAN_MS));
    }
}
