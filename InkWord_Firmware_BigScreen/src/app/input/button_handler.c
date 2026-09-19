/**
 * @file button_handler.c
 * @brief 导航按键扫描实现 —— 自绘板 ADC 分压检测版（GPIO19）
 *
 * 硬件：三按键分压网络汇总到 GPIO19（ADC2_CH8），不同按键拉出
 * 不同原始值窗口；窗口分界宏在 board_config.h，初值为占位，
 * 实测后修正（串口 'a' 诊断，见 console_cmd）。
 *
 * 去抖状态机（每键独立）：候选电平连续 2 采样（100ms）一致才提交
 * 去抖电平；按下沿起计 held_ticks，达 1.5s（30 采样）发长按（一次性）；
 * 释放沿未发过长按则发短按。事件结构体打包入 FreeRTOS 队列（深度 16，
 * 满丢最旧 + 告警，刷新阻塞 3-4s 期间连按排队不丢——小屏 T0.2 同款
 * 语义，大屏全刷更慢，队列化更必要）。
 *
 * ADC 初始化失败时降级为无硬件键（串口命令注入补位），不阻塞启动。
 */
#include "button_handler.h"

#include <string.h>

#include "board_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"

static const char *TAG = "BTN";

#define SCAN_PERIOD_MS   50    /* 轮询周期 */
#define DEBOUNCE_TICKS   2     /* 去抖：候选电平连续采样数（100ms） */
#define LONG_PRESS_TICKS 30    /* 长按阈值 1.5s = 30 x 50ms */
#define QUEUE_DEPTH      16    /* 事件队列深度（满丢最旧） */
#define ADC_SAMPLES      4     /* 每周期采样均值次数（抗噪） */

/* GPIO19 = ADC2_CH8（ESP32-S3）；仅 BIGSCREEN_APP 编入本文件，
 * 与 LAN 固件的 WiFi/ADC2 互斥无关 */
#define BS_BTN_ADC_UNIT   ADC_UNIT_2
#define BS_BTN_ADC_CHAN   ADC_CHANNEL_8

/* 三物理键→导航键映射（board_config.h 宏可配） */
static const nav_key_t k_adc_map[3] = {
    BS_BTN_KEY1, BS_BTN_KEY2, BS_BTN_KEY3,
};

static adc_oneshot_unit_handle_t s_adc = NULL;  /* NULL=ADC 不可用，无硬件键 */

/* ADC2/WiFi 互斥软门（lan_portal 会话用）：true=扫描挂起不采样
 * （GPIO19=ADC2_CH8，WiFi 射频占用期间 ADC2 读取冲突） */
static volatile bool s_scan_paused = false;

/* 队列消息（扫描任务 -> 主任务；文件内私有类型） */
typedef struct {
    nav_key_t id;
    button_event_t ev;
} button_msg_t;

/* 单键去抖状态 */
typedef struct {
    bool pressed;      /* 去抖后电平（true=按下） */
    bool candidate;    /* 上一采样候选电平 */
    int  cand_ticks;   /* 候选电平连续计数 */
    int  held_ticks;   /* 按住时长（按下期每周期 +1） */
    bool long_fired;   /* 长按已触发（释放不再发短按） */
} btn_state_t;

static btn_state_t s_states[NAV_KEY_COUNT];
static QueueHandle_t s_queue = NULL;

/* 事件入队：满丢最旧（保最新交互） */
static void push_event(nav_key_t id, button_event_t ev)
{
    static const char *k_names[] = {"UP", "DOWN", "LEFT", "RIGHT", "CENTER", "SET", "RST"};
    ESP_LOGI(TAG, "按键事件：%s %s", k_names[id],
             ev == BUTTON_EVENT_LONG_PRESS ? "长按" : "短按");
    button_msg_t msg;
    msg.id = id;
    msg.ev = ev;
    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        button_msg_t drop;
        xQueueReceive(s_queue, &drop, 0);   /* 丢最旧再重发 */
        xQueueSend(s_queue, &msg, 0);
        ESP_LOGW(TAG, "event queue full, dropped oldest (%d)", (int)drop.id);
    }
}

/* ADC 原始值窗口判键：返回 0/1/2=键序号，-1=无键。
 * 分界宏为占位初值，待实测修正（board_config.h 注释） */
static int adc_raw_to_key(int raw)
{
    if (raw >= BS_BTN_ADC_TH_NONE) return -1;
    if (raw < BS_BTN_ADC_TH_1_2) return 0;
    if (raw < BS_BTN_ADC_TH_2_3) return 1;
    return 2;
}

/* 单周期采样：4 次均值（诊断与扫描共用，保证实测值=运行判据） */
int button_adc_sample(void)
{
    if (!s_adc) return -1;
    int sum = 0, n = 0;
    for (int i = 0; i < ADC_SAMPLES; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, BS_BTN_ADC_CHAN, &raw) == ESP_OK) {
            sum += raw;
            n++;
        }
    }
    return n > 0 ? sum / n : -1;
}

static void scan_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_scan_paused) {          /* WiFi 会话期挂起（200ms 粗粒度足够） */
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        int raw = button_adc_sample();
        int key = raw >= 0 ? adc_raw_to_key(raw) : -1;
        bool any_pressed = key >= 0;

        for (int i = 0; i < 3; i++) {
            btn_state_t *st = &s_states[i];
            bool level = any_pressed && key == i;

            /* 去抖：候选电平连续一致才提交 */
            if (level != st->candidate) {
                st->candidate = level;
                st->cand_ticks = 1;
            } else if (st->cand_ticks < DEBOUNCE_TICKS) {
                st->cand_ticks++;
            } else if (level != st->pressed) {
                /* 去抖电平翻转沿 */
                st->pressed = level;
                if (level) {                  /* 按下沿：起计长按 */
                    st->held_ticks = 0;
                    st->long_fired = false;
                } else {                    /* 释放沿：未发长按则短按 */
                    if (!st->long_fired)
                        push_event(k_adc_map[i],
                                   BUTTON_EVENT_SHORT_PRESS);
                }
            }

            if (st->pressed) {
                st->held_ticks++;
                if (!st->long_fired &&
                    st->held_ticks >= LONG_PRESS_TICKS) {
                    st->long_fired = true;
                    push_event(k_adc_map[i], BUTTON_EVENT_LONG_PRESS);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(SCAN_PERIOD_MS));
    }
}

int button_handler_init(void)
{
    if (s_queue) return 0;   /* 重入幂等 */

    memset(s_states, 0, sizeof(s_states));

    /* ADC 初始化：GPIO19 = ADC2_CH8，12bit，DB_12 满量程 ~3.3V
     * （分压网络按 3V3 设计，低档位会截断高端段） */
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = BS_BTN_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    if (adc_oneshot_new_unit(&unit_cfg, &s_adc) != ESP_OK) {
        ESP_LOGE(TAG, "ADC 单元初始化失败，降级为串口命令输入");
        s_adc = NULL;
    } else {
        adc_oneshot_chan_cfg_t chan_cfg = {
            .bitwidth = ADC_BITWIDTH_12,
            .atten = ADC_ATTEN_DB_12,
        };
        if (adc_oneshot_config_channel(s_adc, BS_BTN_ADC_CHAN, &chan_cfg)
                != ESP_OK) {
            ESP_LOGE(TAG, "ADC 通道配置失败，降级为串口命令输入");
            s_adc = NULL;
        }
    }

    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(button_msg_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "event queue create failed");
        return -1;
    }

    BaseType_t ok = xTaskCreate(scan_task, "btn_scan", 3 * 1024, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "scan task create failed");
        vQueueDelete(s_queue);
        s_queue = NULL;
        return -1;
    }

    ESP_LOGI(TAG, "按键扫描就绪：ADC@GPIO%d %s（三键分压；串口 a 实测阈值）",
             BS_BTN_ADC_GPIO, s_adc ? "在线" : "降级");
    return 0;
}

bool button_wait(nav_key_t *id, button_event_t *ev, uint32_t timeout_ms)
{
    if (!s_queue || !id || !ev) return false;
    button_msg_t msg;
    if (xQueueReceive(s_queue, &msg, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        *id = msg.id;
        *ev = msg.ev;
        return true;
    }
    return false;
}

bool button_is_pressed(nav_key_t id)
{
    if (id < 0 || id >= NAV_KEY_COUNT) return false;
    for (int i = 0; i < 3; i++)          /* 查物理键→导航映射 */
        if (k_adc_map[i] == id) return s_states[i].pressed;
    return false;                        /* 无物理键映射该导航键 */
}

void button_inject(nav_key_t id, button_event_t ev)
{
    if (!s_queue) return;
    if (id < 0 || id >= NAV_KEY_COUNT) return;
    push_event(id, ev);
}

void button_scan_pause(void)
{
    s_scan_paused = true;
    ESP_LOGW(TAG, "扫描挂起（WiFi 会话期 ADC2 让路）");
}

void button_scan_resume(void)
{
    s_scan_paused = false;
    /* WiFi stop 后 ADC2 仲裁恢复的保险：重配通道（幂等；失败降级串口） */
    if (s_adc) {
        adc_oneshot_chan_cfg_t chan_cfg = {
            .bitwidth = ADC_BITWIDTH_12,
            .atten = ADC_ATTEN_DB_12,
        };
        if (adc_oneshot_config_channel(s_adc, BS_BTN_ADC_CHAN, &chan_cfg)
                != ESP_OK) {
            ESP_LOGE(TAG, "ADC 通道重配失败，降级为串口命令输入");
            s_adc = NULL;
        }
    }
    ESP_LOGW(TAG, "扫描恢复");
}
