/**
 * @file console_cmd.c
 * @brief 串口命令模拟按键实现
 *
 * 命令表（行模式：输入字符 + 回车提交；stdin 走 IDF console VFS）：
 *   u/d/l/r/c/s/t = 上/下/左/右/中/SET/RST 短按
 *   U/D/L/R/C/S/T = 对应键长按
 *   ?  = 重印帮助
 * 事件经 button_inject 入队（满丢最旧同扫描侧），主循环 button_wait
 * 无差别消费——编排层（app_main）与真实硬件路径完全同构。
 *
 * 行内多余字符只取第一个有效字符（快速连打容错）；无法识别的行
 * 静默忽略（刷屏期回显延迟属正常，命令已在队列）。
 */
#include "console_cmd.h"

#include <stdio.h>
#include <unistd.h>
#include <sys/select.h>

#include "button_handler.h"
#include "board_config.h"
#include "epd_gfx.h"
#include "waveform_scanq.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "CONSOLE";

static const struct {
    char cmd;              /* 短按命令（小写） */
    nav_key_t key;
    const char *name;
} k_map[] = {
    { 'u', NAV_UP,     "上" },
    { 'd', NAV_DOWN,   "下" },
    { 'l', NAV_LEFT,   "左" },
    { 'r', NAV_RIGHT,  "右" },
    { 'c', NAV_CENTER, "中" },
    { 's', NAV_SET,    "SET" },
    { 't', NAV_RST,    "RST" },
};
#define MAP_N (sizeof(k_map) / sizeof(k_map[0]))

static void print_help(void)
{
    printf("\n=== 串口按键命令（大小写=短/长按，回车提交）===\n");
    printf("  u/d/l/r/c/s/t : 短按 上/下/左/右/中/SET/RST\n");
    printf("  U/D/L/R/C/S/T : 长按（U=全刷清屏 D=切模式 S=收藏 T=错词本 C=菜单桩）\n");
    printf("  a : ADC 实时监控（GPIO19，200ms 节拍，任意键退出）\n");
    printf("  w : 切换全刷波形 binfast↔GC16（黑不黑/白不白时回退对照）\n");
    printf("  W : 查询全刷波形状态\n");
    printf("  ? : 重印本帮助\n\n");
}

/* ADC 连续实时监控：200ms 节拍打印原始值+判键结果，任意串口输入退出。
 * 操作：敲 a 进入监控 → 分别按住三键观察 raw 值落段 → 任意键退出 →
 * 按段间中线填 board_config.h 阈值宏 */
static void adc_diag(void)
{
    int raw = button_adc_sample();
    if (raw < 0) {
        printf("ADC 不可用（初始化降级）\n");
        return;
    }
    printf("\n=== ADC 实时监控（GPIO19，200ms 节拍；任意键退出）===\n");
    printf("  当前阈值：K1<%d  K2<%d  无键>=%d\n\n",
           BS_BTN_ADC_TH_1_2, BS_BTN_ADC_TH_2_3, BS_BTN_ADC_TH_NONE);

    int n = 0;
    for (;;) {
        struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        if (select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv) > 0)
            break;

        raw = button_adc_sample();
        if (raw < 0) {
            printf("  [%4d] ADC 读取失败\n", ++n);
        } else {
            const char *label;
            if (raw >= BS_BTN_ADC_TH_NONE)      label = "无键";
            else if (raw < BS_BTN_ADC_TH_1_2)   label = "键1(UP)";
            else if (raw < BS_BTN_ADC_TH_2_3)   label = "键2(DOWN)";
            else                                 label = "键3(CENTER)";
            printf("  [%4d] raw=%-4d → %s\n", ++n, raw, label);
        }
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    printf("\n=== 监控结束（已记录 %d 组）===\n", n);
}

static void handle_char(char ch)
{
    if (ch == 'a' || ch == 'A') {
        adc_diag();
        return;
    }
    if (ch == 'w') {
        /* binfast↔GC16 一键互切：真机 A/B 判读（binfast 扫描数未收敛，
         * 黑不黑/白不白签名出现时切回 GC16 即恢复，无需重烧） */
        epd_gfx_set_binfast(!epd_gfx_binfast_enabled());
        return;
    }
    if (ch == 'W') {
        int n1 = 0, n2 = 0;
        binfast_get_params(&n1, &n2);
        printf("全刷波形：%s（binfast 参数 bn1=%d bn2=%d，%d 扫 ≈ %d ms）\n",
               epd_gfx_binfast_enabled() ? "binfast" : "GC16 builtin",
               n1, n2, n1 + n2, (n1 + n2) * 110);
        return;
    }
    if (ch == '?' ) {
        print_help();
        return;
    }
    for (size_t i = 0; i < MAP_N; i++) {
        if (ch == k_map[i].cmd) {
            ESP_LOGI(TAG, "inject: %s 短按", k_map[i].name);
            button_inject(k_map[i].key, BUTTON_EVENT_SHORT_PRESS);
            return;
        }
        if (ch == k_map[i].cmd - 'a' + 'A') {   /* 对应大写 = 长按 */
            ESP_LOGI(TAG, "inject: %s 长按", k_map[i].name);
            button_inject(k_map[i].key, BUTTON_EVENT_LONG_PRESS);
            return;
        }
    }
    /* 无效字符静默忽略 */
}

static void console_task(void *arg)
{
    (void)arg;
    print_help();
    char line[32];
    for (;;) {
        /* fgets 行模式（阻塞至回车）；stdin 就绪由 console_cmd_init
         * 前置的 IDF console 默认 VFS 保证（uart0 行缓冲） */
        if (fgets(line, sizeof(line), stdin) && line[0] != '\0') {
            handle_char(line[0]);
        }
    }
}

void console_cmd_init(void)
{
    static bool s_started = false;
    if (s_started) return;
    s_started = true;

    BaseType_t ok = xTaskCreate(console_task, "console", 4 * 1024, NULL, 3, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "console task create failed");
        return;
    }
    ESP_LOGI(TAG, "串口命令通道就绪（? 查看命令表）");
}
