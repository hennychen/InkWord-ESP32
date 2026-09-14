// 卖家 ED060KD1-EpdiyV7 demo 完整复刻（ES108FC 1920x1080 版）。
// 来源：Info/ED060KD1-EpdiyV7示例和操作说明/ED060KD1-EpdiyV7/main.c
// 库：components/epdiy = 卖家 epdiy2 原库（仅加 IDF 5.5 编译层适配，
//     驱动逻辑与卖家发布完全一致，未引入本项目任何管线修复）。
// 目的：验证卖家组合（原库 + 本流程）在此屏的真实表现，与
//       epdiy_inkword_fixed 组件的修复效果做 A/B 对照。
//
// 与卖家 main.c 的刻意差异（其余逐行对齐）：
//   1. epd_init 目标屏 ES120 -> ES108FC（本机面板，定义取自卖家 main.c L54-61）；
//   2. epd_poweron/off 的 Arduino digitalWrite(46,x) 宏 -> 本项目
//      board_config.h 的 BS_EPD_POWER_EN（同为 GPIO46，等价替换）；
//   3. 末尾 esp_deep_sleep_start() -> 打印完成日志并循环（便于反复观察）；
//   4. 字体实体在 test_basic.c（fonts/firasans_20.h 为非 static const），
//      此处 extern 引用避免重定义链接冲突。
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

#include <epdiy.h>

#include "sdkconfig.h"

#include "driver/gpio.h"

#include "board_config.h"  // BS_EPD_POWER_EN（=46，与 demo 的 digitalWrite(46,*) 一致）

#include "demo/img_beach.h"
#include "demo/img_board.h"
#include "demo/img_zebra.h"

static const char* TAG = "demo_seller";

// 卖家 demo 原文：#define WAVEFORM EPD_BUILTIN_WAVEFORM
#define WAVEFORM EPD_BUILTIN_WAVEFORM

// 卖家 demo 原文为 Arduino 宏 digitalWrite(46,1/0)，等价替换为本项目电源脚
#define demo_poweron() gpio_set_level(BS_EPD_POWER_EN, 1)
#define demo_poweroff() gpio_set_level(BS_EPD_POWER_EN, 0)

// [run91] 局刷产品对策：每 8 次局刷插一次全屏 GC16 重置（抑灰染，见循环注释）
#define RUN91_RESET_K 8

// 卖家 main.c L54-61：ES108FC 屏定义。
// [run40] 17→3 对照实验（证实卖家库两速率均死锁，速率非主变量）。
// [run42] 3→5：产能扩容后回 5MHz（NUM_RENDER_THREADS 2→4，消除 run37
// 的 16% 垫错行）。波形仍 ED047TC1（卖家定义）。
const EpdDisplay_t ES108FC = {
    .width = 1920,
    .height = 1080,
    .bus_width = 16,
    .bus_speed = 5,
    .default_waveform = &epdiy_ED047TC1,
    .display_type = DISPLAY_TYPE_GENERIC,
};

static EpdiyHighlevelState hl;

// 字体实体在 test_basic.c（fonts/firasans_20.h，非 static const 全局）
extern const EpdFont FiraSans_20;

static void idf_setup() {
    epd_init(&epd_board_v7, &ES108FC, EPD_LUT_64K);

    // 卖家 demo 原文：epd_set_vcom(1560)。v7 板无软件 VCOM 通道，库会打
    // "board does not support" 错误后返回（demo 注释亦说明可删，实际由
    // 板载电位器决定）。保留以完全复刻卖家行为；本面板排线标签为 -2.45V。
    epd_set_vcom(1560);

    hl = epd_hl_init(WAVEFORM);

    // Default orientation is EPD_ROT_LANDSCAPE
    epd_set_rotation(EPD_ROT_LANDSCAPE);

    printf(
        "Dimensions after rotation, width: %d height: %d\n\n", epd_rotated_display_width(),
        epd_rotated_display_height()
    );

    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);
}

static void delay_ms(uint32_t millis) {
    vTaskDelay(millis / portTICK_PERIOD_MS);
}

static inline void checkError(enum EpdDrawError err) {
    if (err != EPD_DRAW_SUCCESS) {
        ESP_LOGE(TAG, "draw error: %X", err);
    } else {
        ESP_LOGI(TAG, "draw ok");
    }
}

static void draw_progress_bar(int x, int y, int width, int percent, uint8_t* fb) {
    const uint8_t white = 0xFF;
    const uint8_t black = 0x0;

    EpdRect border = {
        .x = x,
        .y = y,
        .width = width,
        .height = 20,
    };
    epd_fill_rect(border, white, fb);
    epd_draw_rect(border, black, fb);

    EpdRect bar = {
        .x = x + 5,
        .y = y + 5,
        .width = (width - 10) * percent / 100,
        .height = 10,
    };

    epd_fill_rect(bar, black, fb);

    checkError(epd_hl_update_area(&hl, MODE_DU, epd_ambient_temperature(), border));
}

static void idf_loop() {
    // select the font based on display width
    const EpdFont* font;
    if (epd_width() < 1000) {
        // 卖家此处用 FiraSans_12；本面板 1920 宽恒走 20 号，此处与卖家
        // 保持分支结构，12 号字体未编入（实体引用见顶部说明）
        font = &FiraSans_20;
    } else {
        font = &FiraSans_20;
    }

    uint8_t* fb = epd_hl_get_framebuffer(&hl);

    demo_poweron();
    epd_clear();
    int temperature = epd_ambient_temperature();
    demo_poweroff();

    printf("current temperature: %d\n", temperature);

    epd_fill_circle(30, 30, 15, 0, fb);
    int cursor_x = epd_rotated_display_width() / 2;
    int cursor_y = epd_rotated_display_height() / 2 - 100;

    EpdFontProperties font_props = epd_font_properties_default();
    font_props.flags = EPD_DRAW_ALIGN_CENTER;

    char srotation[32];
    sprintf(srotation, "Loading demo...\nRotation: %d", epd_get_rotation());

    epd_write_string(font, srotation, &cursor_x, &cursor_y, fb, &font_props);

    int bar_x = epd_rotated_display_width() / 2 - 200;
    int bar_y = epd_rotated_display_height() / 2;

    demo_poweron();

    checkError(epd_hl_update_screen(&hl, MODE_GL16, temperature));
    demo_poweroff();

    for (int i = 0; i < 6; i++) {
        demo_poweron();
        draw_progress_bar(bar_x, bar_y, 400, i * 10, fb);
        demo_poweroff();
    }

    cursor_x = epd_rotated_display_width() / 2;
    cursor_y = epd_rotated_display_height() / 2 + 100;

    epd_write_string(
        font, "Just kidding,\n this is a demo animation", &cursor_x, &cursor_y, fb, &font_props
    );
    demo_poweron();
    checkError(epd_hl_update_screen(&hl, MODE_GL16, temperature));
    demo_poweroff();

    for (int i = 0; i < 6; i++) {
        demo_poweron();
        draw_progress_bar(bar_x, bar_y, 400, 50 - i * 10, fb);
        demo_poweroff();
        vTaskDelay(1);
    }

    cursor_y = epd_rotated_display_height() / 2 + 200;
    cursor_x = epd_rotated_display_width() / 2;

    EpdRect clear_area = {
        .x = 0,
        .y = epd_rotated_display_height() / 2 + 100,
        .width = epd_rotated_display_width(),
        .height = 300,
    };

    epd_fill_rect(clear_area, 0xFF, fb);

    epd_write_string(
        font, "Now let's look at some pictures.", &cursor_x, &cursor_y, fb, &font_props
    );
    demo_poweron();
    checkError(epd_hl_update_screen(&hl, MODE_GL16, temperature));
    demo_poweroff();

    delay_ms(1000);

    epd_hl_set_all_white(&hl);

    EpdRect zebra_area = {
        .x = epd_rotated_display_width() / 2 - img_zebra_width / 2,
        .y = epd_rotated_display_height() / 2 - img_zebra_height / 2,
        .width = img_zebra_width,
        .height = img_zebra_height,
    };

    epd_draw_rotated_image(zebra_area, img_zebra_data, fb);
    demo_poweron();
    checkError(epd_hl_update_screen(&hl, MODE_GC16, temperature));
    demo_poweroff();

    delay_ms(5000);

    EpdRect board_area = {
        .x = epd_rotated_display_width() / 2 - img_board_width / 2,
        .y = epd_rotated_display_height() / 2 - img_board_height / 2,
        .width = img_board_width,
        .height = img_board_height,
    };

    epd_draw_rotated_image(board_area, img_board_data, fb);
    cursor_x = epd_rotated_display_width() / 2;
    cursor_y = board_area.y;
    font_props.flags |= EPD_DRAW_BACKGROUND;
    epd_write_string(font, "v Thats the V2 board. v", &cursor_x, &cursor_y, fb, &font_props);

    demo_poweron();
    checkError(epd_hl_update_screen(&hl, MODE_GC16, temperature));
    demo_poweroff();

    delay_ms(5000);
    epd_hl_set_all_white(&hl);

    EpdRect border_rect = {
        .x = 20,
        .y = 20,
        .width = epd_rotated_display_width() - 40,
        .height = epd_rotated_display_height() - 40};
    epd_draw_rect(border_rect, 0, fb);

    cursor_x = 50;
    cursor_y = 100;

    epd_write_default(
        font,
        "> 16 color grayscale\n"
        "> ~250ms - 1700ms for full frame draw\n"
        "> Use with 6\" or 9.7\" EPDs\n"
        "> High-quality font rendering\n"
        "> Partial update\n"
        "> Arbitrary transitions with vendor waveforms",
        &cursor_x, &cursor_y, fb
    );

    EpdRect img_beach_area = {
        .x = 0,
        .y = epd_rotated_display_height() - img_beach_height,
        .width = img_beach_width,
        .height = img_beach_height,
    };

    epd_draw_rotated_image(img_beach_area, img_beach_data, fb);

    demo_poweron();
    checkError(epd_hl_update_screen(&hl, MODE_GC16, temperature));
    demo_poweroff();

    // 卖家原文此处 esp_deep_sleep_start()；改为完成日志 + 短歇后循环，
    // 便于反复观察画面（.ino 的 loop() 语义亦为无限循环）。
    ESP_LOGW(TAG, "demo 一轮完成，5s 后重跑（卖家原文此处深睡）");
    delay_ms(5000);
}

// [run44] 行映射标定帧：量化 fb 坐标 → 屏显位置的系统性映射。
// 布局：顶/底 16 行黑带、每 100 行 4 行细线、左右 8px 竖条、中央十字。
// 判读：黑带是否在顶/底→垂直方向；细线数与间距→垂直缩放；
// 竖条是否贴边→水平映射；十字是否居中→整体偏移。
// （run25 同款帧当时数据链未修（EOF 减半），结果不可信；数据链修复后重测。）
// [run45] 屏竖放确认：内容旋转 90° 显示，判读时黑带/细线在照片横轴、
// 竖条/十字竖线在照片纵轴。两次拍照均错过 8s 观察窗（拍到 demo 帧），
// 改为常驻循环重画，彻底取消拍照时机约束。
static void draw_line_map_frame(uint8_t* fb) {
    const int W = 1920, H = 1080;
    
    // 先全白背景
    memset(fb, 0xFF, W * H / 2);
    
    // 顶部和底部黑条
    EpdRect top = {0, 0, W, 16};
    epd_fill_rect(top, 0x0, fb);
    EpdRect bottom = {0, H - 16, W, 16};
    epd_fill_rect(bottom, 0x0, fb);
    
    // 左右黑边
    EpdRect left = {0, 0, 8, H};
    epd_fill_rect(left, 0x0, fb);
    EpdRect right = {W - 8, 0, 8, H};
    epd_fill_rect(right, 0x0, fb);
    
    // 十字交叉线
    EpdRect hcross = {0, H / 2 - 2, W, 4};
    epd_fill_rect(hcross, 0x0, fb);
    EpdRect vcross = {W / 2 - 2, 0, 4, H};
    epd_fill_rect(vcross, 0x0, fb);
    
    // 每 100 行绘制水平线和数字标记
    EpdFontProperties props = epd_font_properties_default();
    for (int y = 0; y < H; y += 100) {
        // 水平线
        EpdRect line = {0, y, W, 2};
        epd_fill_rect(line, 0x0, fb);
        
        // 数字标记（左侧）
        char label[16];
        snprintf(label, sizeof(label), "%d", y);
        int cursor_x = 20;
        int cursor_y = y + 20;
        epd_write_string(&FiraSans_20, label, &cursor_x, &cursor_y, fb, &props);
    }
    
    // 中心大文字标记
    int cx = W / 2 - 60;
    int cy = H / 2 + 20;
    epd_write_string(&FiraSans_20, "CENTER", &cx, &cy, fb, &props);
}

void demo_seller_task(void* arg) {
    // 电源脚输出模式（demo 全程 GPIO 直控，不走库电源回调）
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BS_EPD_POWER_EN,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);

    idf_setup();

    // [run70] 精确恢复 run45 语义（最后已知良好状态）：
    //   循环内无 epd_clear、无 poweron/off；电源仅任务_start 上电一次；
    //   set_all_white → 画标定帧 → GC16 更新 → 5s → 重复。
    //   墨水屏双稳：首轮真实更新后图像常驻，后续轮 diff 空 no-op 无害。
    // [run88] 局部刷新测试：第 0 轮全屏标定帧作背景，之后六步循环
    //   分区 update_area：P0 batch0 内 / P1 跨 y=1000 但高<1000(单批) /
    //   P2 batch1 内 / P3 跨边界且高>1000(局部帧含批边界, 验 z=2 补偿
    //   门限按帧相对行计) / P4 同 P0 区但 MODE_DU 快刷 / P5 小文本区。
    //   每步白底+黑框+计数文字+奇偶块保证 diff 非空。
    // [run91] 产品对策验证：未更新区灰染 ∝ 连续 nop 全扫次数（run88 ~45 循环
    //   灰染），每 RUN91_RESET_K 次局刷插一次全屏 GC16 重置归零累积（强驱动
    //   覆写泵电荷）；重置帧不推进六步序列，fb 为累积全图 update_screen 直取。
    int frame_count = 0;
    int partial_step = 0;
    int since_reset = 0;

    // [run89 实验后回退] 曾试 epd_set_vcom(1560)+epd_poweron() 软件校准 VCOM：
    //   本克隆板 I2C 控制链不存在（卖家原库同注释），epd_poweron 内
    //   tps 写入 ESP_ERR_INVALID_STATE abort 无限重启。灰染为硬件 VCOM
    //   固定偏移特性，软件不可修 → 回退 run88 稳定电源路径。
    demo_poweron();
    delay_ms(500);

    while (1) {
        ESP_LOGI(TAG, "Frame %d", frame_count);
        uint8_t* fb = epd_hl_get_framebuffer(&hl);
        enum EpdDrawError err;

        if (frame_count == 0) {
            epd_hl_set_all_white(&hl);
            draw_line_map_frame(fb);
            err = epd_hl_update_screen(&hl, MODE_GC16, epd_ambient_temperature());
        } else if (since_reset >= RUN91_RESET_K) {
            ESP_LOGW(TAG, "run91 reset: full GC16 after %d partials", since_reset);
            err = epd_hl_update_screen(&hl, MODE_GC16, epd_ambient_temperature());
            since_reset = 0;
        } else {
            static const EpdRect areas[] = {
                {300, 200, 700, 300},   // P0 batch0 内
                {300, 850, 700, 300},   // P1 跨 y=1000, 高 300 单批
                {1100, 1000, 700, 70},  // P2 batch1 内
                {1400, 20, 300, 1040},  // P3 跨边界 高 1040 → 局部帧含批边界
                {300, 200, 700, 300},   // P4 同 P0 区, MODE_DU
                {300, 600, 400, 40},    // P5 小文本区
            };
            int step = partial_step % 6;
            EpdRect area = areas[step];
            enum EpdDrawMode mode = (step == 4) ? MODE_DU : MODE_GL16;

            epd_fill_rect(area, 0xFF, fb);
            epd_draw_rect(area, 0x0, fb);
            EpdRect blk = {area.x + area.width - 60, area.y + 10, 40, 40};
            epd_fill_rect(blk, (partial_step / 6) % 2 ? 0x0 : 0xFF, fb);
            char label[32];
            snprintf(label, sizeof(label), "P%d n=%d", step, frame_count);
            int cx = area.x + 10;
            int cy = area.y + 30;
            EpdFontProperties props = epd_font_properties_default();
            epd_write_string(&FiraSans_20, label, &cx, &cy, fb, &props);

            ESP_LOGI(TAG, "partial step %d area=%d,%d,%d,%d mode=%d", step, area.x, area.y,
                     area.width, area.height, (int)mode);
            err = epd_hl_update_area(&hl, mode, epd_ambient_temperature(), area);
            partial_step++;
            since_reset++;
        }

        if (err != EPD_DRAW_SUCCESS) {
            ESP_LOGE(TAG, "draw error: %X", err);
        } else {
            ESP_LOGI(TAG, "draw ok");
        }

        frame_count++;
        delay_ms(3000);
    }
}
