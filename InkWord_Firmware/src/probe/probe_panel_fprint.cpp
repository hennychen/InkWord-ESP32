/**
 * @file probe_panel_fprint.cpp
 * @brief 当前上机屏的寄存器指纹实测探针（2026-09-19，3.1" bring-up 起因）
 *
 * 背景：真机启动日志显示 auto-detect 以「0x71 读回 0x13 → 第一阶 OTP
 * 唯一命中」把选屏判给 opm021eb_bw（122x250），而构建 env 钉的是
 * gdeq031t10_uc8253。opm021eb 的 otp_signature 注释本身就写着
 * 「0x71 FLG … WFT0290 同族应答值」——FLG 是状态位不是身份位，
 * 拿它当指纹会把整个 UC 族互相错认。本探针实测三件事：
 *   ① 0x71 读数是否稳定、是否随 0x12 软复位变化（判「身份」还是「状态」）
 *   ② UC 族分辨率读 0x65/0x66 在真机上到底给什么值（auto-detect
 *      第三阶分辨率维度的可用性）
 *   ③ 同一批寄存器换用「单命令单读」与「连读多字节」两种口径对照，
 *      排除读时序造成的假值
 * 结论回填 epd_panel.c auto-detect 注释与相关 desc。
 */
#include <Arduino.h>
#include "../gpio_config.h"

/* 位带 SPI 读（口径同 epd_bus.cpp::bus_diag_read_status：命令阶段 DC=0
 * 发 8 位，数据阶段 DC=1 且 MOSI 转输入交 COG 驱动，Mode0 上升沿采样）。
 * 探针 env 只编译本文件，故自带实现而非调用 L0 */
static uint8_t xfer_read_byte(void)
{
    uint8_t v = 0;
    for (int i = 7; i >= 0; i--) {
        digitalWrite(EPD_SCK_PIN, HIGH);
        delayMicroseconds(2);
        v = (uint8_t)((v << 1) | (digitalRead(EPD_MOSI_PIN) ? 1 : 0));
        digitalWrite(EPD_SCK_PIN, LOW);
        delayMicroseconds(2);
    }
    return v;
}

/* n=1 走生产同款事务；n>1 在单 CS 事务内连读（dummy 字节会体现在序列里） */
static void reg_read(const char *tag, uint8_t cmd, int n, bool delay_50ms)
{
    digitalWrite(EPD_CS_PIN, LOW);
    digitalWrite(EPD_DC_PIN, LOW);
    digitalWrite(EPD_SCK_PIN, LOW);
    for (int i = 7; i >= 0; i--) {
        digitalWrite(EPD_MOSI_PIN, (cmd >> i) & 0x01);
        delayMicroseconds(2);
        digitalWrite(EPD_SCK_PIN, HIGH);
        delayMicroseconds(2);
        digitalWrite(EPD_SCK_PIN, LOW);
    }
    digitalWrite(EPD_DC_PIN, HIGH);
    pinMode(EPD_MOSI_PIN, INPUT);
    if (delay_50ms) delay(50);
    uint8_t got[4] = {0};
    for (int i = 0; i < n; i++) got[i] = xfer_read_byte();
    digitalWrite(EPD_CS_PIN, HIGH);
    digitalWrite(EPD_DC_PIN, LOW);
    pinMode(EPD_MOSI_PIN, OUTPUT);

    Serial.printf("  %-10s 0x%02X ->", tag, cmd);
    for (int i = 0; i < n; i++) Serial.printf(" %02X", got[i]);
    Serial.println();
}

/* 探针 env 不编 epd_bus.cpp，命令写同样自带（DC=0 事务） */
static void bus_cmd_local(uint8_t c)
{
    digitalWrite(EPD_CS_PIN, LOW);
    digitalWrite(EPD_DC_PIN, LOW);
    digitalWrite(EPD_SCK_PIN, LOW);
    for (int i = 7; i >= 0; i--) {
        digitalWrite(EPD_MOSI_PIN, (c >> i) & 0x01);
        delayMicroseconds(2);
        digitalWrite(EPD_SCK_PIN, HIGH);
        delayMicroseconds(2);
        digitalWrite(EPD_SCK_PIN, LOW);
    }
    digitalWrite(EPD_CS_PIN, HIGH);
}

static void busy_level(const char *tag)
{
    int hi = 0;
    for (int i = 0; i < 5; i++) {
        if (digitalRead(EPD_BUSY_PIN)) hi++;
        delay(10);
    }
    Serial.printf("  %-10s BUSY %d/5 HIGH\n", tag, hi);
}

static void hw_reset(int pulse_ms)
{
    digitalWrite(EPD_RESET_PIN, HIGH);
    delay(200);
    digitalWrite(EPD_RESET_PIN, LOW);
    delay(pulse_ms);
    digitalWrite(EPD_RESET_PIN, HIGH);
    delay(300);   /* 静置后再采样，避开自检忙窗 */
}

static void fingerprint(const char *round)
{
    Serial.printf("\n=== %s ===\n", round);
    busy_level("idle");
    reg_read("FLG", 0x71, 1, false);
    reg_read("FLG x3", 0x71, 3, false);
    reg_read("status", 0x2F, 1, true);   /* SSD16xx 版本读（UC 屏应无应答） */
    reg_read("UC 0x65", 0x65, 1, false);
    reg_read("UC 0x66", 0x66, 1, false);
    reg_read("UC 0x61", 0x61, 3, false); /* UC8253 resolution setting */
    reg_read("UC 0x61b", 0x61, 1, false);
    reg_read("SSD 0x44", 0x44, 1, false);
    reg_read("SSD 0x45", 0x45, 2, false);
}

void setup()
{
    Serial.begin(115200);
    delay(3000);
    Serial.println("\n### panel fingerprint probe (3.1\" bring-up, 2026-09-19) ###");

    pinMode(EPD_RESET_PIN, OUTPUT);
    pinMode(EPD_CS_PIN, OUTPUT);
    pinMode(EPD_DC_PIN, OUTPUT);
    pinMode(EPD_MOSI_PIN, OUTPUT);
    pinMode(EPD_SCK_PIN, OUTPUT);
    pinMode(EPD_BUSY_PIN, INPUT);
    digitalWrite(EPD_CS_PIN, HIGH);
    digitalWrite(EPD_DC_PIN, LOW);

    /* 轮 1：RST 后（auto-detect 的采样点，10ms 脉宽与其同口径） */
    hw_reset(10);
    fingerprint("round1: after RST(10ms)");

    /* 轮 2：0x12 软复位后——FLG 若为状态寄存器，POR/POF 位会翻转 */
    bus_cmd_local(0x12);
    delay(100);
    fingerprint("round2: after SWRESET 0x12");

    /* 轮 3：连续两次 0x71 读之间夹 1s，看温度/电源位漂移 */
    Serial.println("\n=== round3: FLG drift @1s ===");
    for (int i = 0; i < 4; i++) {
        reg_read("FLG", 0x71, 1, false);
        delay(1000);
    }

    /* 轮 4：50ms RST 脉宽对照（desc rst_pulse_ms 敏感性） */
    hw_reset(50);
    fingerprint("round4: after RST(50ms)");

    Serial.println("\n=== done ===");
}

void loop() { delay(1000); }
