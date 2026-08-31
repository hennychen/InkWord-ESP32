/**
 * @file probe_gxepd2_uc8151d.cpp
 * @brief 二十七轮（2026-08-31）对照实验：GxEPD2 标准 UC8151D 驱动直驱
 *        OPM021EB —— 排除手写 panel_opm021eb 驱动嫌疑
 *
 * 背景：手写驱动 REG LUT（PSR bit5=1）局刷八轮证伪（0x10/0x13 写入
 * 被吞、0x12 空转 50ms）；但 OTP 模式（0x1F）与 partial window
 * （0x90/0x91/0x92，二十六轮 A 线）全链路正常。两种可能：
 *   a) ESL 定制 COG 封锁 REG LUT 模式 RAM 写入（硬件锁）
 *   b) 手写驱动 REG 模式序列有隐性 bug
 * 本探针用 GxEPD2_290_T5D（真 UC8151D 参考类，128x296，作者按官方
 * datasheet 写就、社区长期验证）的完整标准序列重走 REG LUT 局刷路径：
 *   - 局刷后屏显 A/B 交替 + partial 计时 ~750ms → 手写驱动有 bug（b）
 *   - 局刷后屏显不变（停在首次全刷图 A）→ COG 硬件锁实锤（a）
 *
 * 引脚（gpio_config.h 同源）：SCK=7 MOSI=8 DC=9 CS=10 BUSY=12(LOW=忙)
 * RST=13。数据按 128x296 发送，OPM021EB 有效区 122x250 显示图案主体。
 *
 * 用法：pio run -e opm021eb-probe -t upload && pio device monitor -b 115200
 * 测完烧回：pio run -e inkword-s3-opm021eb -t upload
 */
#include <Arduino.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <epd/GxEPD2_290_T5D.h>   /* 库头在 GxEPD2/src/epd/ 子目录 */

/* EVK011 + OPM021EB 实板引脚（与 InkWord gpio_config.h EPD 段一致） */
static const int PIN_SCK  = 7;
static const int PIN_MOSI = 8;
static const int PIN_DC   = 9;
static const int PIN_CS   = 10;
static const int PIN_BUSY = 12;   /* LOW = 忙（UC8151D 同极性） */
static const int PIN_RST  = 13;

static GxEPD2_BW<GxEPD2_290_T5D, GxEPD2_290_T5D::HEIGHT> g_disp(
    GxEPD2_290_T5D(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));

static bool s_state = false;       /* A/B 图案交替 */

/* 画测试图：大字母 + 棋盘边条（TINY 屏远看可辨） */
static void draw_pattern(bool which)
{
    g_disp.setFullWindow();
    g_disp.firstPage();
    do {
        g_disp.fillScreen(GxEPD_WHITE);
        g_disp.setTextColor(GxEPD_BLACK);
        g_disp.setTextSize(9);                 /* ~72px 大字（默认 8px 字模） */
        g_disp.setCursor(38, 100);
        g_disp.print(which ? 'B' : 'A');
        /* 底部棋盘条：A=左半黑块，B=右半黑块（局刷是否生效的强视觉信号） */
        for (int i = 0; i < 8; i++) {
            int x = which ? 68 + (i % 4) * 16 : 4 + (i % 4) * 16;
            g_disp.fillRect(x, 4 + (i / 4) * 16, 12, 12, GxEPD_BLACK);
        }
    } while (g_disp.nextPage());
}

void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.printf("\n[PROBE] GxEPD2_290_T5D (UC8151D) on OPM021EB\n");

    SPI.begin(PIN_SCK, -1, PIN_MOSI, PIN_CS);  /* GPIO matrix 任意引脚 */

    /* 本版 GxEPD2 API：引脚在构造函数已给，init(serial_diag_bitrate,
     * initial, ...)——115200 开库内诊断日志（每命令可见），initial=false
     * 跳过库自动白屏全刷（下方手动全刷画 A 作基准） */
    g_disp.init(115200, false);
    Serial.printf("[PROBE] init done, full-refresh %dms expected\n",
                  (int)GxEPD2_290_T5D::full_refresh_time);

    /* 第一步：标准全刷画 A（局刷失效时的“冻结画面”基准） */
    uint32_t t0 = millis();
    draw_pattern(false);
    g_disp.display(true);                      /* full */
    Serial.printf("[PROBE] FULL refresh took %ums (pattern A shown)\n",
                  (unsigned)(millis() - t0));

    delay(5000);                               /* 留 5s 观察全刷图案 */
    Serial.printf("[PROBE] start partial loop (watch screen: A/B alternate?)\n");
}

void loop()
{
    /* 局刷路径：setPartialWindow 全窗 + display(false) → GxEPD2 内部
     * _Init_Part（REG LUT + PSR 0xBF + 五张 LUT）+ 写双 RAM + 0x12 */
    s_state = !s_state;
    draw_pattern(s_state);
    g_disp.setPartialWindow(0, 0, g_disp.width(), g_disp.height());
    /* paged 重画到 partial window：nextPage 尾页触发 _Update_Part */
    g_disp.firstPage();
    do {
        g_disp.fillScreen(GxEPD_WHITE);
        g_disp.setTextColor(GxEPD_BLACK);
        g_disp.setTextSize(9);
        g_disp.setCursor(38, 100);
        g_disp.print(s_state ? 'B' : 'A');
        for (int i = 0; i < 8; i++) {
            int x = s_state ? 68 + (i % 4) * 16 : 4 + (i % 4) * 16;
            g_disp.fillRect(x, 4 + (i / 4) * 16, 12, 12, GxEPD_BLACK);
        }
    } while (g_disp.nextPage());

    Serial.printf("[PROBE] PARTIAL (%s) done @%ums\n",
                  s_state ? "B" : "A", (unsigned)millis());
    delay(5000);
}
