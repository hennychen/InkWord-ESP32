/* probe_opm_seq.cpp —— OPM021EB 全刷序列滞后一帧定位探针（三十四轮）
 *
 * 现象：回滚固件（uc_refresh_bw 路径）翻词仍滞后一帧（第一次下翻
 * 屏显旧词）。假说：0x12 后 BUSY 释放 ≠ 波形会话关闭，紧跟的
 * 0x02 POF 打断会话收尾 → 下一轮 0x12 续跑旧目标（断点续刷语义，
 * 与打断法滞后同机制）。26 轮 partial-win 路径（含 0x92）正确，
 * 说明序列差异决定行为。
 *
 * 三组序列 × 3 帧（帧图案 = N 条横带，N=1/2/3，肉眼可数）：
 *   A 组（复刻 uc_refresh_bw）：PON→0x13→0x12→等BUSY→立即 0x02
 *   B 组（POF 前延时 500ms）：PON→0x13→0x12→等BUSY→delay500→0x02
 *   C 组（不 POF）：PON→0x13→0x12→等BUSY→（保持上电）→下一帧
 * 判据：每组内屏显 1→2→3 依次正确 = 无滞后；显示 1→1→2 = 滞后。
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

static void wait_busy(uint32_t timeout_ms)
{
    uint32_t t0 = millis();
    while (digitalRead(PIN_BUSY) == LOW) {   /* BUSY LOW = 忙 */
        if (millis() - t0 > timeout_ms) break;
        delay(1);
    }
    delay(2);
}

/* 全刷行宽 15B/行（122px → TRES 口径，write_ram_frame 同款重排） */
static uint8_t s_frame[15 * 250];

static void fill_bars(int n)
{
    /* N 条横带：带高 30px，带距 30px，从 y=25 起；bit=1 白底黑带 */
    memset(s_frame, 0xFF, sizeof(s_frame));
    for (int b = 0; b < n; b++) {
        int y0 = 25 + b * 60;
        for (int y = y0; y < y0 + 30 && y < 250; y++)
            for (int x = 0; x < 122; x++)
                s_frame[y * 15 + (x >> 3)] &=
                    (uint8_t)~(0x80 >> (x & 7));
    }
}

static void send_frame(void)
{
    cmd(0x13);
    digitalWrite(PIN_CS, LOW);
    digitalWrite(PIN_DC, HIGH);
    for (size_t i = 0; i < sizeof(s_frame); i++)
        bb_byte_out(s_frame[i]);
    digitalWrite(PIN_CS, HIGH);
}

/* 完整 init（照 uc_init：RST + 首次 0x12 初始化刷） */
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
    cmd(0x12);                     /* 首次 0x12：内部初始化刷 */
    wait_busy(10000);
    cmd(0x02);
    wait_busy(1000);
}

/* 单帧全刷 + 0x92 收尾（变体）：with_win=1 时带 0x91/0x90
 * （复刻 26 轮 partial-win 全链），否则只加 0x92（最小修复） */
static void frame_seq_92(int bars, int with_win)
{
    uint32_t t0 = millis();
    cmd(0x04);
    wait_busy(5000);
    fill_bars(bars);
    if (with_win) {
        cmd(0x91);                 /* partial in */
        cmd(0x90);                 /* partial window（全屏） */
        dat(0x00); dat(0x7F);
        dat(0x00); dat(0x00);
        dat(0x00); dat(0xF9);
        dat(0x01);
    }
    send_frame();
    cmd(0x12);
    wait_busy(10000);
    cmd(0x92);                     /* partial out：会话关闭假说 */
    wait_busy(2000);
    cmd(0x02);
    wait_busy(1000);
    Serial.printf("[SEQ] bars=%d win=%d took %ums\n",
                  bars, with_win, (unsigned)(millis() - t0));
}

void setup()
{
    Serial.begin(115200);
    delay(300);
    bb_init_pins();
    cog_init();
    Serial.println("[SEQ] init done");

    Serial.println("[SEQ] === D2: 0x13/0x12/BUSY/0x92/0x02 (minimal) ===");
    frame_seq_92(1, 0);
    delay(3000);
    frame_seq_92(2, 0);
    delay(3000);
    frame_seq_92(3, 0);
    delay(3000);

    Serial.println("[SEQ] === D1: full partial-win chain (26-round replica) ===");
    frame_seq_92(1, 1);
    delay(3000);
    frame_seq_92(2, 1);
    delay(3000);
    frame_seq_92(3, 1);
    delay(3000);

    Serial.println("[SEQ] sweep complete");
}

void loop() { delay(1000); }
