/* probe_opm_seq2.cpp —— OPM021EB 滞后一帧二分定位探针（三十七轮）
 *
 * 矛盾矩阵：34 轮 D1（PON/0x91/0x90/0x13(15B)/0x12/0x92/wait2s/0x02）
 * 显示正确（终态 3）；但正式驱动 36 轮（同链 + 0x21 对 + 寄存器组 +
 * LUT 五表 + 16B 行宽）滞后一帧。本轮从 D1 基线逐项加回，定位破坏者：
 *   E1 = D1 精确复刻（15B/行）          —— 基线复现性
 *   E2 = E1 但 16B/行（窗口口径对齐）   —— 行宽变量
 *   E3 = E2 + 0x21/0x44→0x21/0x00 序列对 —— 十二轮副作用对
 *   E4 = E3 + PSR/TRES/VCOM/CDI/0x11 + LUT 五表（=36 轮正式序列）
 * 每组 2 帧（横带 1 条 → 2 条）：正确 = 屏显 1→2 交替；滞后 = 恒显。
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

static uint8_t s_frame[16 * 250];

static void fill_bars(int n)
{
    memset(s_frame, 0xFF, sizeof(s_frame));
    for (int b = 0; b < n; b++) {
        int y0 = 25 + b * 60;
        for (int y = y0; y < y0 + 30 && y < 250; y++)
            for (int x = 0; x < 122; x++)
                s_frame[y * 16 + (x >> 3)] &=
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

static const uint8_t LUT_VCOM_DC[44] = {
    0x00, 0x28, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00,
};
static const uint8_t LUT_X[42] = {
    0x00, 0x28, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static void frame_e(int bars, int row_bytes, int with_21, int with_regs)
{
    uint32_t t0 = millis();
    cmd(0x04);
    wait_busy(5000);

    if (with_21) {
        cmd(0x21); dat(0x44);
        cmd(0x21); dat(0x00);
    }
    if (with_regs) {
        cmd(0x00); dat(0x1F);
        cmd(0x61); dat(122); dat(0); dat(250);
        cmd(0x82); dat(0x08);
        cmd(0x50); dat(0x17);
        cmd(0x11); dat(0x03);
        cmd(0x20); dat_burst(LUT_VCOM_DC, sizeof(LUT_VCOM_DC));
        cmd(0x21); dat_burst(LUT_X, sizeof(LUT_X));
        cmd(0x22); dat_burst(LUT_X, sizeof(LUT_X));
        cmd(0x23); dat_burst(LUT_X, sizeof(LUT_X));
        cmd(0x24); dat_burst(LUT_X, sizeof(LUT_X));
        cmd(0x04);
        wait_busy(5000);
    }

    fill_bars(bars);
    cmd(0x91);
    cmd(0x90);
    dat(0x00); dat(0x7F);
    dat(0x00); dat(0x00);
    dat(0x00); dat(0xF9);
    dat(0x01);

    cmd(0x13);                     /* 窗口数据：row_bytes/行 x 250 行 */
    dat_burst(s_frame, (size_t)row_bytes * 250);

    cmd(0x12);
    wait_busy(10000);
    cmd(0x92);
    wait_busy(2000);
    cmd(0x02);
    wait_busy(1000);
    Serial.printf("[SEQ2] bars=%d rb=%d x21=%d regs=%d took %ums\n",
                  bars, row_bytes, with_21, with_regs,
                  (unsigned)(millis() - t0));
}

void setup()
{
    Serial.begin(115200);
    delay(300);
    bb_init_pins();
    cog_init();
    Serial.println("[SEQ2] init done");

    Serial.println("[SEQ2] === E1: D1 replica, 15B/row ===");
    frame_e(1, 15, 0, 0);
    delay(3000);
    frame_e(2, 15, 0, 0);
    delay(3000);

    Serial.println("[SEQ2] === E2: 16B/row (window alignment) ===");
    frame_e(1, 16, 0, 0);
    delay(3000);
    frame_e(2, 16, 0, 0);
    delay(3000);

    Serial.println("[SEQ2] === E3: E2 + 0x21 pair ===");
    frame_e(1, 16, 1, 0);
    delay(3000);
    frame_e(2, 16, 1, 0);
    delay(3000);

    Serial.println("[SEQ2] === E4: E3 + regs + LUT (36-round seq) ===");
    frame_e(1, 16, 1, 1);
    delay(3000);
    frame_e(2, 16, 1, 1);
    delay(3000);

    Serial.println("[SEQ2] sweep complete");
}

void loop() { delay(1000); }
