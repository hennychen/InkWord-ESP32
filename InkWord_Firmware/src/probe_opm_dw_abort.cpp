/* probe_opm_dw_abort.cpp —— OPM021EB 打断+双写组合探针（四十轮）
 *
 * 遗留可选项实验（39 轮定稿后）：双写（0x10+0x13 同帧）使 0x12 波形
 * 目标为当前帧（38 轮 H1 定案）；若波形执行中 X ms 处 0x02 POF 打断，
 * 屏显应为部分驱动的当前帧——对比度够则快刷复活（翻词 ~1.3s）。
 * 与"OTP 无快档"不矛盾：ED057TC1 打断法快刷同为全刷波形截短。
 *
 * 判据（37/38 轮教训：组内逐帧 + 条带图，不用 checkerboard）：
 *   G0 对照：双写全刷 ×3（bars 1/2/3）——基线，应 1→2→3
 *   G1~G4：X=2000/1000/500/250ms 打断 ×3（bars 1/2/3）
 *     每组 1→2→3 且条带黑白分明 = 该 X 档快刷成立（看下限）
 *     恒旧帧/滞后 = 打断下双写失效；淡灰可辨 = 部分成（对比度档）
 *   settle：全刷 bars=4 清残影收尾
 * 序列：PON → 0x10 → 0x13 → 0x12 → delay(X) → 0x02 POF（不等 BUSY）。
 * 全程不 RST（RST 清状态回慢波形，ED057TC1 经验）；POF 后 PON busy
 * 可能挂死但有超时兜底，不碍 0x12（31 轮实证）。
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

/* 双写帧。abort_ms < 0 = 全刷（等 BUSY 完整波形）；
 * >= 0 = 波形执行 abort_ms 后 0x02 POF 打断（不等 BUSY） */
static void frame_dw(int bars, int abort_ms)
{
    uint32_t t0 = millis();
    cmd(0x04);
    wait_busy(5000);
    fill_bars(bars);
    cmd(0x10);                      /* DTM1 old（同帧，差分基准） */
    dat_burst(s_frame, sizeof(s_frame));
    cmd(0x13);                      /* DTM2 new */
    dat_burst(s_frame, sizeof(s_frame));
    cmd(0x12);
    if (abort_ms < 0) {
        wait_busy(10000);           /* 完整波形 ~2965ms */
    } else {
        delay((uint32_t)abort_ms);  /* 波形执行 Xms 处打断 */
        cmd(0x02);                  /* POF，不等 BUSY */
        wait_busy(1000);
    }
    Serial.printf("[DWAB] bars=%d abort=%d took %ums\n",
                  bars, abort_ms, (unsigned)(millis() - t0));
}

void setup()
{
    Serial.begin(115200);
    delay(300);
    bb_init_pins();
    cog_init();
    Serial.println("[DWAB] init done");

    Serial.println("[DWAB] === G0: dual-write FULL (baseline, expect 1-2-3) ===");
    frame_dw(1, -1); delay(2500);
    frame_dw(2, -1); delay(2500);
    frame_dw(3, -1); delay(2500);

    Serial.println("[DWAB] === G1: abort 2000ms ===");
    frame_dw(1, 2000); delay(2500);
    frame_dw(2, 2000); delay(2500);
    frame_dw(3, 2000); delay(2500);

    Serial.println("[DWAB] === G2: abort 1000ms ===");
    frame_dw(1, 1000); delay(2500);
    frame_dw(2, 1000); delay(2500);
    frame_dw(3, 1000); delay(2500);

    Serial.println("[DWAB] === G3: abort 500ms ===");
    frame_dw(1, 500); delay(2500);
    frame_dw(2, 500); delay(2500);
    frame_dw(3, 500); delay(2500);

    Serial.println("[DWAB] === G4: abort 250ms ===");
    frame_dw(1, 250); delay(2500);
    frame_dw(2, 250); delay(2500);
    frame_dw(3, 250); delay(2500);

    Serial.println("[DWAB] === settle: full refresh x2 ===");
    frame_dw(4, -1); delay(2500);
    frame_dw(4, -1); delay(2500);
    Serial.println("[DWAB] sweep complete");
}

void loop() { delay(1000); }
