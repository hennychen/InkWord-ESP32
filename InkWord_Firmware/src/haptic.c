/**
 * @file haptic.c
 * @brief 震动马达触觉反馈实现 (PRD 5.4 事件表)
 *
 * 关断链：esp_timer 一次性回调（esp_timer task 上下文，gpio_set_level
 * 任务级安全）；双短震经 phase 状态机链式调度。硬件未接线时调用
 * 全部安全（GPIO41 空操作，仅日志可见事件流）。
 */
#include "haptic.h"
#include "gpio_config.h"
#include "debug_log.h"

#include "driver/gpio.h"
#include "esp_timer.h"

static const char *TAG = "HAPTIC";

/* 活动电平：常见马达模块（MOS 高有效）为 1；
 * 低有效模块（板载上拉 + NMOS）改 0 */
#ifndef HAPTIC_ACTIVE_HIGH
#define HAPTIC_ACTIVE_HIGH 1
#endif

/* 双短震相位（0=空闲，1=单脉冲中，2/3/4=双脉冲三阶段） */
enum { PH_IDLE = 0, PH_SINGLE, PH_DBL_ON1, PH_DBL_GAP, PH_DBL_ON2 };

static esp_timer_handle_t s_timer;
static uint8_t  s_phase = PH_IDLE;
static uint16_t s_on_ms = 0;   /* 双脉冲段时长 */
static uint16_t s_gap_ms = 0;  /* 双脉冲间隔 */

static void motor_set(bool on)
{
#if HAPTIC_ACTIVE_HIGH
    gpio_set_level(HAPTIC_PIN, on ? 1 : 0);
#else
    gpio_set_level(HAPTIC_PIN, on ? 0 : 1);
#endif
}

static void timer_cb(void *arg)
{
    (void)arg;
    switch (s_phase) {
    case PH_SINGLE:                     /* 单脉冲到时：关断 */
    case PH_DBL_ON2:                    /* 双脉冲第二段到时：关断 */
        motor_set(false);
        s_phase = PH_IDLE;
        break;
    case PH_DBL_ON1:                    /* 第一段到时：停机进入间隔 */
        motor_set(false);
        s_phase = PH_DBL_GAP;
        esp_timer_start_once(s_timer, (uint64_t)s_gap_ms * 1000);
        break;
    case PH_DBL_GAP:                    /* 间隔到时：第二段起震 */
        motor_set(true);
        s_phase = PH_DBL_ON2;
        esp_timer_start_once(s_timer, (uint64_t)s_on_ms * 1000);
        break;
    default:
        break;
    }
}

int haptic_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << HAPTIC_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    motor_set(false);   /* 上电确保关断（避免 MOS 悬空误导通） */

    const esp_timer_create_args_t args = {
        .callback = timer_cb,
        .name = "haptic",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &s_timer) != ESP_OK) {
        s_timer = NULL;
        LOG_W("haptic timer create failed, motor disabled");
        return -1;
    }
    LOG_I("haptic motor ready on GPIO%d (%s-active)",
          HAPTIC_PIN, HAPTIC_ACTIVE_HIGH ? "high" : "low");
    return 0;
}

void haptic_pulse(uint16_t on_ms)
{
    if (!s_timer || on_ms == 0) return;
    esp_timer_stop(s_timer);           /* 重叠事件：重启当前脉冲（后到优先） */
    s_phase = PH_SINGLE;
    motor_set(true);
    esp_timer_start_once(s_timer, (uint64_t)on_ms * 1000);
}

void haptic_pulse2(uint16_t on_ms, uint16_t gap_ms)
{
    if (!s_timer || on_ms == 0) return;
    esp_timer_stop(s_timer);
    s_on_ms  = on_ms;
    s_gap_ms = gap_ms;
    s_phase  = PH_DBL_ON1;
    motor_set(true);
    esp_timer_start_once(s_timer, (uint64_t)on_ms * 1000);
}

void haptic_off(void)
{
    if (s_timer) esp_timer_stop(s_timer);   /* 双短震链式调度一并终止 */
    s_phase = PH_IDLE;
    motor_set(false);
}

void haptic_event(haptic_event_t ev)
{
    /* 时长表 = PRD_V2.1 §5.4 触觉反馈列 */
    switch (ev) {
    case HAPTIC_KEYPRESS: haptic_pulse(20);  break;  /* 按键按下瞬间 */
    case HAPTIC_REVIEW:   haptic_pulse(30);  break;  /* 自评提交 */
    case HAPTIC_MODE:     haptic_pulse(50);  break;  /* 进入/退出模式 */
    case HAPTIC_ERROR:    haptic_pulse(100); break;  /* 错误操作（边界） */
    case HAPTIC_PASS:     haptic_pulse2(30, 80); break; /* 跟读通过（P4 预留） */
    case HAPTIC_FAIL:     haptic_pulse(100); break;  /* 跟读失败（P4 预留） */
    default: break;
    }
}
