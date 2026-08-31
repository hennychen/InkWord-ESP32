/* probe_opm_dram.cpp —— OPM021EB 双 RAM 显示目标假说探针（三十八轮）
 *
 * 现象链：33~37 轮全部"滞后一帧"；37 轮探针 8 帧全显旧图案（0x13
 * 疑似未进显示 RAM）。假说：UC8151D 双 RAM（DTM1=0x10 old /
 * DTM2=0x13 new）的 DRF(0x12) 显示目标是 DTM1 —— 只写 0x13 时
 * DTM1 永不更新（RST 后首次 0x12 内部同步一次 = boot 首屏正确的
 * 来源），此后永远滞后。WFT0290/UC8253 家族双 RAM 差分是同源机制。
 *
 * 三组 × 3 帧（横带 1/2/3），纯全刷序列（无窗口/无 0x21/无 LUT）：
 *   H3（对照）：只写 0x13 + 0x12        —— 预期滞后（1,1,2）
 *   H1：0x10 + 0x13 双写（同帧数据）+ 0x12 —— 预期依次正确（1,2,3）
 *   H2：只写 0x10 + 0x12                —— DTM1 单写变体
 * 判据：H1 依次 1→2→3 = 双 RAM 假说定案，修复 = 驱动加 0x10 双写。
 */
#include <Arduino.h>

static const int PIN_SCK = 7, PIN_MOSI = 8, PIN_DC = 9,
                 PIN_CS = 10, PIN_BUSY = 12, PIN_RST = 13;

static void bb_init_pins(void)
{
    pinMode(PIN_DC, OUTPUT);
    pinMode(PIN_CS, OUTPUT);
    pinMode(PIN_RST, OUTPUT);
    pinMode(PIN_BUSY, INPUT);
    digitalWrite(PIN_CS, HIGH);
    pinMode(PIN_MOSI, OUTPUT);
    pinMode(PIN_SCK, OUTPUT);
    digitalWrite(PIN_SCK, LOW);
}

static void bb_byte_out(uint8_t v)
{
    for (int i = 7; i >= 0; i--) {
        digitalWrite(PIN_MOSI, (v >> i) & 1);
        delayMicroseconds(2);
        digitalWrite(PIN_SCK, HIGH);
        delayMicroseconds(2);
        digitalWrite(PIN_SCK, LOW);
    }
}

static void cmd(uint8_t c)
{
    digitalWrite(PIN_CS, LOW);
    digitalWrite(PIN_DC, LOW);
    bb_byte_out(c);
    digitalWrite(PIN_CS, HIGH);
}

static void dat(uint8_t d)
{
    digitalWrite(PIN_CS, LOW);
    digitalWrite(PIN_DC, HIGH);
    bb_byte_out(d);
    digitalWrite(PIN_CS, HIGH);
}

static void dat_burst(const uint8_t *p, size_t n)
{
    digitalWrite(PIN_CS, LOW);
    digitalWrite(PIN_DC, HIGH);
    while (n--) bb_byte_out(*p++);
    digitalWrite(PIN_CS, HIGH);
}

static void wait_busy(uint32_t timeout_ms)
{
    uint32_t t0 = millis();
    while (digitalRead(PIN_BUSY) == LOW) {
        if (millis() - t0 > timeout_ms) break;
        delay(1);
    }
    delay(2);
}

/* 全刷口径 15B/行（TRES 122px，write_ram_frame 同款） */
static uint8_t s_frame[15 * 250];

static void fill_bars(int n)
{
    memset(s_frame, 0xFF, sizeof(s_frame));
    for (int b = 0; b < n; b++) {
        int y0 = 25 + b * 60;
        for (int y = y0; y < y0 + 30 && y < 250; y++)
            for (int x = 0; x < 122; x++)
                s_frame[y * 15 + (x >> 3)] &=
                    (uint8_t)~(0x80 >> (x & 7));
    }
}

static void cog_init(void)
{
    digitalWrite(PIN_RST, HIGH); delay(200);
    digitalWrite(PIN_RST, LOW);  delay(10);
    digitalWrite(PIN_RST, HIGH); delay(10);
    wait_busy(5000);
    cmd(0x00); dat(0x1F);
    cmd(0x61); dat(122); dat(0); dat(250);
    cmd(0x50); dat(0x97);
    cmd(0x1C); dat(0x80);
    cmd(0x04);
    wait_busy(5000);
    cmd(0x12);
    wait_busy(10000);
    cmd(0x02);
    wait_busy(1000);
}

/* mode: 0=只 0x13（对照） 1=0x10+0x13 双写 2=只 0x10 */
static void frame_h(int bars, int mode)
{
    uint32_t t0 = millis();
    cmd(0x04);
    wait_busy(5000);
    fill_bars(bars);
    if (mode == 1 || mode == 2) {
        cmd(0x10);                  /* DTM1 old */
        dat_burst(s_frame, sizeof(s_frame));
    }
    if (mode == 0 || mode == 1) {
        cmd(0x13);                  /* DTM2 new */
        dat_burst(s_frame, sizeof(s_frame));
    }
    cmd(0x12);
    wait_busy(10000);
    cmd(0x02);
    wait_busy(1000);
    Serial.printf("[DRAM] bars=%d mode=%d took %ums\n",
                  bars, mode, (unsigned)(millis() - t0));
}

void setup()
{
    Serial.begin(115200);
    delay(300);
    bb_init_pins();
    cog_init();
    Serial.println("[DRAM] init done");

    Serial.println("[DRAM] === H3: 0x13 only (control, expect lag) ===");
    frame_h(1, 0); delay(3000);
    frame_h(2, 0); delay(3000);
    frame_h(3, 0); delay(3000);

    Serial.println("[DRAM] === H1: 0x10+0x13 dual write (hypothesis) ===");
    frame_h(1, 1); delay(3000);
    frame_h(2, 1); delay(3000);
    frame_h(3, 1); delay(3000);

    Serial.println("[DRAM] === H2: 0x10 only ===");
    frame_h(1, 2); delay(3000);
    frame_h(2, 2); delay(3000);
    frame_h(3, 2); delay(3000);

    Serial.println("[DRAM] sweep complete");
}

void loop() { delay(1000); }
