// InkWord_Firmware_BigScreen 入口（Phase 0 骨架）
// 路径分派：bigscreen-probe env（-D BIGSCREEN_PROBE）走八步 bring-up
// 序列（方案 §3.4）；默认 env 仅安全初始化 + 全白清屏后挂起。
#include <stdio.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/panel_es108fc.h"
#include "driver/temp_monitor.h"

#if defined(BIGSCREEN_SELLER_DEMO)
#include "demo/demo_seller.h"
#elif defined(BIGSCREEN_LAN)
#include "lan/lan_image.h"
#elif defined(BIGSCREEN_PROBE)
#include "test_basic.h"
#elif defined(BIGSCREEN_APP)
#include "app/app_main.h"
#elif defined(BIGSCREEN_TUNE)
#include "tune/tune_task.h"
extern void tune_process_command(char cmd);
#endif

static const char* TAG = "bigscreen";

// 串口命令处理任务（tune 环境专用）
#if defined(BIGSCREEN_TUNE)
static void uart_command_task(void* arg) {
    (void)arg;
    ESP_LOGI(TAG, "串口命令任务启动（n=下一组 p=上一组 r=重刷 i=信息 w=波形 h=帮助）");

    while (1) {
        int ch = getchar();
        if (ch != EOF && ch > 0) {
            tune_process_command((char)ch);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
#endif

void app_main(void) {
    ESP_LOGW(TAG, "InkWord BigScreen（espidf + epdiy V7 / ES108FC1C1-RHY）");
    ESP_LOGW(TAG, "本固件仅面向大屏新板，禁止烧录至现有小屏设备（防误烧红线）");

    // 帧缓冲依赖 PSRAM（1920x1080@4bpp ≈ 1MB），先报告容量
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PSRAM: total=%zu bytes, free=%zu bytes", psram_total, psram_free);

    // 温度监控（规格书 §10 工作温度 0~50°C，30 秒采样一次）
    temp_monitor_start(30000);

#if defined(BIGSCREEN_SELLER_DEMO)
    ESP_LOGW(TAG, "BIGSCREEN_SELLER_DEMO：跑卖家 epdiy2 原库 + ED060KD1 demo 流程（ES108FC）");
    // 栈与优先级对齐 bringup 任务（printf/heap 打印开销 + feed 忙等轮转约束）
    if (xTaskCreatePinnedToCore(demo_seller_task, "demo_seller",
                                1 << 14, NULL, configMAX_PRIORITIES - 1,
                                NULL, tskNO_AFFINITY) != pdPASS) {
        ESP_LOGE(TAG, "demo_seller 任务创建失败");
    }
#elif defined(BIGSCREEN_LAN)
    ESP_LOGW(TAG, "BIGSCREEN_LAN：WiFi AP + HTTP 图片上传显示（适配里程碑 M1）");
    if (xTaskCreatePinnedToCore(lan_image_task, "lan_image",
                                1 << 14, NULL, configMAX_PRIORITIES - 1,
                                NULL, tskNO_AFFINITY) != pdPASS) {
        ESP_LOGE(TAG, "lan_image 任务创建失败");
    }
#elif defined(BIGSCREEN_PROBE)
    ESP_LOGW(TAG, "BIGSCREEN_PROBE：进入八步 bring-up 序列（每步间留观察窗）");
    // [InkWord 修复] bring-up 主控提到与 epdiy feed 线程同优先级
    // （configMAX_PRIORITIES-1）：feed 忙等只做同优先级 vTaskDelay(0) 轮转，
    // 低优先级主控会在帧完成后永久饿死（frame_done 就绪也无任务可调度，
    // 静默挂死，真机实证）。同优先级后轮转点即上核，吞吐无损。
    if (xTaskCreatePinnedToCore(bringup_run_all_task, "bringup",
                                1 << 13, NULL, configMAX_PRIORITIES - 1,
                                NULL, tskNO_AFFINITY) != pdPASS) {
        ESP_LOGE(TAG, "bring-up 任务创建失败");
    }
#elif defined(BIGSCREEN_APP)
    ESP_LOGW(TAG, "BIGSCREEN_APP：学习闭环主应用（init 链 + 事件循环）");
    // 面板/词库/状态机 init 链在 app_task 内（分段计时日志）；
    // 栈与优先级对齐 lan/demo 先例，主控低于 feed 线程一档避免饿死
    if (app_main_start() != 0) {
        ESP_LOGE(TAG, "学习闭环主任务创建失败");
    }
#elif defined(BIGSCREEN_TUNE)
    ESP_LOGW(TAG, "BIGSCREEN_TUNE：波形调参测试（标准测试图 + 参数切换）");
    // 先初始化面板，再启动调参任务
    esp_err_t err = panel_es108fc_safe_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "面板安全初始化失败: %s", esp_err_to_name(err));
    } else {
        if (xTaskCreatePinnedToCore(tune_task, "tune",
                                    1 << 14, NULL, configMAX_PRIORITIES - 1,
                                    NULL, tskNO_AFFINITY) != pdPASS) {
            ESP_LOGE(TAG, "调参任务创建失败");
        }
        // 启动串口命令任务
        if (xTaskCreate(uart_command_task, "uart_cmd", 4096, NULL, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "串口命令任务创建失败");
        }
    }
#else
    esp_err_t err = panel_es108fc_safe_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "面板安全初始化失败: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "面板安全初始化完成（全白清屏已执行），骨架验证通过");
    }
#endif

    // bring-up 阶段无业务循环，挂起省电等待复位
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
