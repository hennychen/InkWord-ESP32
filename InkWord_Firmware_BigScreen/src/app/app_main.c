/**
 * @file app_main.c
 * @brief 大屏学习闭环应用主任务实现
 *
 * init 链（分段计时，串口观察各阶段开销——计划测试点 2）：
 *   NVS → SPIFFS 书库（storage 分区，失败不阻塞）→ 内嵌词库
 *   （word_parser_load_mem 直吃 rodata，PSRAM 词池）
 *   → learning_state（NVS sparse 恢复）→ panel 安全初始化 → epd_gfx
 *   → study_mode（last_mode 恢复）→ button/console → page_router
 *   → 首帧词卡（阅读引擎惰性初始化，首进阅读页时才建页表）
 *
 * 事件循环：button_wait 100ms 节拍消费（真实 GPIO / 串口注入同队列）；
 * 5 分钟无操作进待机页（任意键回词卡）；page_router_dispatch_button
 * 分发到 base 页编排（word_card 按键语义，小屏 word_view_on_button
 * 精简版：无菜单/设置/portal/lan 栈页，长按中键为菜单桩）。
 *
 * 按键编排（V2.1 交互总表大屏版）：
 *   短按：上/下=词内释义页→翻词，中=发音(桩)，SET=遮蔽/揭晓，
 *         RST=回首，左=自评忘了(Q1)，右=自评简单(Q5)+墨封联动
 *   长按：上=全刷清屏，下=切模式(FLASH<->REVIEW)，SET=收藏★，
 *         RST=错词本进出，中=菜单桩
 */
#include "app_main.h"

#include <stddef.h>

#include "button_handler.h"
#include "console_cmd.h"
#include "debug_log.h"
#include "driver/panel_es108fc.h"
#include "epd_gfx.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "learning_state.h"
#include "lan_portal.h"
#include "menu_ui.h"
#include "nvs_flash.h"
#include "page_router.h"
#include "standby_page.h"
#include "storage_manager.h"
#include "study_mode_machine.h"
#include "word_card_ui.h"
#include "word_parser.h"

static const char *TAG = "APP";

/* ---- 内嵌词库（CMake EMBED_FILES；路径斜线→下划线进符号） ---- */
extern const uint8_t _binary_app_data_default_words_json_start[];
extern const uint8_t _binary_app_data_default_words_json_end[];

/* 词池容量上限（PSRAM 预算：2407 词现状 ~2.4MB，上限 3000 词
 * ~3.2MB + 解析期 DOM ~2.4MB + fb 1MB + 画布 0.26MB < 8MB） */
#define WORD_POOL_MAX 3000

#define STANDBY_TIMEOUT_MS (5 * 60 * 1000)   /* 待机：5 分钟无操作 */

static WordEntry *s_pool = NULL;   /* 词池（PSRAM） */

/* ---- base 页：词卡渲染 + 按键编排 ---- */

static bool base_on_button(nav_key_t id, button_event_t event)
{
    study_mode_t m = study_mode_current();

    if (event == BUTTON_EVENT_LONG_PRESS) {
        switch (id) {
        case NAV_CENTER:
            menu_ui_enter();
            return true;
        case NAV_UP:
            /* ghost-clear：back 置黑构造全屏 diff → GC16 白区全驱驱白
             * （清灰染/残影；重渲同内容 diff 空不扫描——run92，旧实现
             * 无效已修 2026-09-16） */
            ESP_LOGI(TAG, "user requested ghost-clear full refresh");
            page_router_render_top();   /* 画布先同步当前页 */
            epd_gfx_force_refresh();
            return true;
        case NAV_DOWN:
            study_mode_switch_next();
            page_router_render_top();
            return true;
        case NAV_SET:
            /* 收藏/取消当前词；收藏视图内=移出序列（after_uncollect
             * 收缩钳位，清空自动归位 FLASH）；墨封录内=启封移出 */
            if (m == MODE_MASTERED) {
                learning_state_toggle_master(study_mode_current_word_index());
                study_mode_after_master();
                page_router_render_top();
                return true;
            }
            learning_state_toggle_collect(study_mode_current_word_index());
            if (m == MODE_COLLECTION) study_mode_after_uncollect();
            page_router_render_top();
            return true;
        case NAV_RST:
            /* 临时视图内=退出回闪卡；base=进错词本（无错词拒绝） */
            if (m == MODE_WRONGBOOK) {
                study_mode_exit_wrongbook();
                page_router_render_top();
                return true;
            }
            if (m == MODE_COLLECTION) {
                study_mode_exit_collection();
                page_router_render_top();
                return true;
            }
            if (m == MODE_MASTERED) {
                study_mode_exit_mastered();
                page_router_render_top();
                return true;
            }
            if (!study_mode_enter_wrongbook()) {
                ESP_LOGW(TAG, "无错词，拒绝进错词本");
                return true;
            }
            page_router_render_top();
            return true;
        default:
            return true;
        }
    }
    if (event != BUTTON_EVENT_SHORT_PRESS) return true;

    switch (id) {
    case NAV_UP:
        if (word_card_ui_mean_page_step(-1)) return true;  /* 释义词内翻页 */
        study_mode_handle_action(0);   /* prev */
        return true;
    case NAV_DOWN:
        if (word_card_ui_mean_page_step(+1)) return true;
        study_mode_handle_action(1);   /* next */
        return true;
    case NAV_CENTER:
        study_mode_handle_action(3);   /* speak（音频门控桩恒跳过） */
        return true;
    case NAV_SET:
        study_mode_handle_action(2);   /* confirm：遮蔽/揭晓 */
        return true;
    case NAV_RST:
        study_mode_reset_cursor();     /* 回首 */
        return true;
    case NAV_LEFT: {
        /* 自评「忘记」Q1（FSRS 入 learning_state；错词本内 Q1 连错
         * 累计，答对路径见 RIGHT） */
        int wi = study_mode_current_word_index();
        if (wi < 0) return true;
        learning_state_apply_quality(wi, 1);
        study_mode_after_quality(1);
        study_mode_after_due_review();   /* REVIEW 序列收缩 */
        page_router_render_top();
        return true;
    }
    case NAV_RIGHT: {
        /* 自评「简单」Q5 + 墨封联动（小屏 2026-09-04 同语义：用户
         * 认为简单=已掌握，toggle 幂等）+ 各视图序列收缩 */
        int wi = study_mode_current_word_index();
        if (wi < 0) return true;
        learning_state_apply_quality(wi, 5);
        learning_state_toggle_master(wi);
        study_mode_after_quality(5);
        study_mode_after_master();
        study_mode_after_due_review();
        page_router_render_top();
        return true;
    }
    default:
        return true;
    }
}

static const page_t s_base_page = {
    .name = "word_card",
    .render = word_card_ui_render,
    .on_button = base_on_button,
    .enter = NULL,
    .exit = NULL,
    .owns_display = false,
};

/* ---- init 链 ---- */

static bool init_chain(void)
{
    int64_t t_boot = esp_timer_get_time();

    /* 1. NVS（learning_state/study_mode 持久化底座） */
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 状态异常，擦除重初始化（%s）", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init 失败: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "[1/8] NVS 就绪（%lld ms）",
             (esp_timer_get_time() - t0) / 1000);

    /* 2. SPIFFS 书库（storage 分区 → /storage；失败不阻塞，阅读器
     * 回退内置演示书——挂载失败自动格式化重挂，首次烧录必经） */
    t0 = esp_timer_get_time();
    if (storage_init() != 0)
        ESP_LOGW(TAG, "SPIFFS 书库挂载失败（阅读器将回退演示书）");
    ESP_LOGI(TAG, "[2/8] 书库挂载（%lld ms）",
             (esp_timer_get_time() - t0) / 1000);

    /* 3. 内嵌词库（rodata 直吃无拷贝；DOM 解析期 PSRAM 峰值见
     * word_parser.c PSRAM hooks 注释） */
    t0 = esp_timer_get_time();
    s_pool = heap_caps_malloc(sizeof(WordEntry) * WORD_POOL_MAX,
                              MALLOC_CAP_SPIRAM);
    if (!s_pool) {
        ESP_LOGE(TAG, "词池分配失败（%d B）",
                 (int)(sizeof(WordEntry) * WORD_POOL_MAX));
        return false;
    }
    size_t json_len = (size_t)(_binary_app_data_default_words_json_end -
                               _binary_app_data_default_words_json_start);
    int n = word_parser_load_mem(
        (const char *)_binary_app_data_default_words_json_start, json_len,
        s_pool, WORD_POOL_MAX);
    if (n <= 0) {
        ESP_LOGE(TAG, "内嵌词库解析失败（%d B）", (int)json_len);
        return false;
    }
    ESP_LOGI(TAG, "[3/8] 词库装载：%d 词（%lld ms）", n,
             (esp_timer_get_time() - t0) / 1000);

    /* 4. 学习状态（NVS sparse 恢复：评分/收藏/墨封跨重启） */
    t0 = esp_timer_get_time();
    learning_state_init(n, "");
    ESP_LOGI(TAG, "[4/8] 学习状态就绪（%lld ms）",
             (esp_timer_get_time() - t0) / 1000);

    /* 5. 面板安全初始化（PSRAM 检查→epd_init→VCOM→全白清屏） */
    t0 = esp_timer_get_time();
    err = panel_es108fc_safe_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "面板安全初始化失败: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "[5/8] 面板就绪（%lld ms）",
             (esp_timer_get_time() - t0) / 1000);

    /* 6. 绘图层（1bpp PSRAM 画布） */
    t0 = esp_timer_get_time();
    if (epd_gfx_init() != 0) {
        ESP_LOGE(TAG, "绘图层初始化失败");
        return false;
    }
    ESP_LOGI(TAG, "[6/8] 绘图层就绪（%lld ms）",
             (esp_timer_get_time() - t0) / 1000);

    /* 7. 学习模式 + 输入通道（真实 GPIO / 串口命令同队列） */
    t0 = esp_timer_get_time();
    study_mode_init();
    if (button_handler_init() != 0) {
        ESP_LOGE(TAG, "按键初始化失败");
        return false;
    }
    console_cmd_init();
    ESP_LOGI(TAG, "[7/8] 模式+输入就绪（%lld ms）",
             (esp_timer_get_time() - t0) / 1000);

    /* 8. 页面路由 + 首帧词卡 */
    t0 = esp_timer_get_time();
    page_router_init(&s_base_page);
    page_router_render_top();
    ESP_LOGI(TAG, "[8/8] 首帧上屏（%lld ms；boot 总计 %lld ms）",
             (esp_timer_get_time() - t0) / 1000,
             (esp_timer_get_time() - t_boot) / 1000);
    return true;
}

/* ---- 事件循环 ---- */

static void app_task(void *arg)
{
    (void)arg;
    if (!init_chain()) {
        ESP_LOGE(TAG, "init 链失败，任务挂起（串口命令通道可用）");
        vTaskSuspend(NULL);
    }

    uint32_t idle_ms = 0;
    bool standby = false;
    for (;;) {
        nav_key_t id;
        button_event_t ev;
        if (button_wait(&id, &ev, 100)) {
            idle_ms = 0;
            if (standby) {
                /* 待机页任意键回词卡（按键本身消费不穿透）；断电唤醒：
                 * 上电 + 升压稳定窗（lan_image L1159 同款 500ms）+
                 * 强驱首帧（电源刚恢复，GC16 全驱重置物理态最稳） */
                standby = false;
                panel_power_on();
                vTaskDelay(pdMS_TO_TICKS(500));
                page_router_render_top();   /* 画布同步（DU 局刷） */
                epd_gfx_force_refresh();    /* GC16 全驱覆盖 */
                continue;
            }
            page_router_dispatch_button(id, ev);
        } else {
            idle_ms += 100;
            /* LAN 会话期不待机：WiFi/传图在跑（屏电不能断），且传图
             * 交互发生在浏览器侧、无本机按键——不能靠 idle 计时判活跃 */
            if (lan_portal_active()) idle_ms = 0;
            if (!standby && idle_ms >= STANDBY_TIMEOUT_MS) {
                standby = true;
                standby_page_render();
                /* 待机断电（run48/49 禁频繁开关电，分钟级低频安全）：
                 * 消除静置期 VCOM 偏置累积（未更新区灰染源）+ 省电 */
                panel_power_off();
                ESP_LOGI(TAG, "待机：屏电已断（任意键唤醒）");
            }
        }
    }
}

int app_main_start(void)
{
    static bool s_started = false;
    if (s_started) return 0;
    s_started = true;

    /* 栈预算：init 链 cJSON 解析（DOM 递归）+ 渲染调用链；16KB 对齐
     * lan_image/demo 先例（printf/heap 打印开销） */
    if (xTaskCreatePinnedToCore(app_task, "app_main", 1 << 14, NULL,
                                configMAX_PRIORITIES - 2, NULL,
                                tskNO_AFFINITY) != pdPASS) {
        ESP_LOGE(TAG, "app_main 任务创建失败");
        return -1;
    }
    return 0;
}
