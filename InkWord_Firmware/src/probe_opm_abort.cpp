/* probe_opm_abort.cpp —— OPM021EB 打断法快刷探针（三十一轮）
 *
 * 灵感来源：ED057TC1（IL0371）v4.7~v4.10 实证的打断法——波形执行中
 * 0x02 POF 打断，翻转进度从打断点继承（RST 会清除回慢波形，故全程
 * 不 RST，仅 POF/PON 轮转）。OPM021EB 的 2965ms OTP 全刷波形执行中
 * 打断从未实测过（历史上八轮试的是 REG 模式 50ms 空转，非波形打断）。
 *
 * 序列（每档 X）：
 *   0x04 PON（超时兜底——ED057TC1 经验：打断后 PON busy 可能挂死
 *   但不碍 0x12）→ 0x13 棋盘帧（相位交替）→ 0x12 → delay(X) →
 *   0x02 POF 打断（不等 BUSY）→ 立即下一轮。
 *
 * 观察要点：棋盘是否肉眼可辨（打断点前的翻转已发生）、对比度随 X
 * 衰减规律、多次打断后累计效果。结束跑一次完整 0x12 清残影。
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
    while (digitalRead(PIN_BUSY) == LOW) {   /* 本板 BUSY LOW = 忙 */
        if (millis() - t0 > timeout_ms) break;
        delay(1);
    }
    delay(2);
}

static uint8_t s_frame[16 * 250];

static void fill_checkerboard(int phase)
{
    for (int y = 0; y < 250; y++)
        for (int b = 0; b < 16; b++)
            s_frame[y * 16 + b] =
                (uint8_t)((((b ^ (y >> 3)) & 1) ^ phase) ? 0xFF : 0x00);
}

/* 打断轮：PON → 帧 → 0x12 → Xms → POF。返回本轮墙钟耗时 */
static uint32_t abort_round(int phase, uint32_t x_ms)
{
    uint32_t t0 = millis();
    cmd(0x04);                       /* PON（超时兜底，不挂死） */
    wait_busy(300);
    fill_checkerboard(phase);
    cmd(0x13);
    dat_burst(s_frame, sizeof(s_frame));
    cmd(0x12);                       /* 波形启动 */
    delay(x_ms);                     /* 波形执行 Xms */
    cmd(0x02);                       /* POF 打断（不等 BUSY） */
    return millis() - t0;
}

static const uint16_t XMS[] = { 2000, 1000, 500, 250, 125 };

void setup()
{
    Serial.begin(115200);
    delay(300);
    bb_init_pins();

    /* RST + 完整 init（照正式驱动：首刷完成内部初始化） */
    digitalWrite(PIN_RST, HIGH); delay(200);
    digitalWrite(PIN_RST, LOW);  delay(10);
    digitalWrite(PIN_RST, HIGH); delay(10);
    wait_busy(5000);
    Serial.println("[ABT] COG reset ok");
    cmd(0x00); dat(0x1F);
    cmd(0x61); dat(122); dat(0); dat(250);
    cmd(0x50); dat(0x97);
    cmd(0x1C); dat(0x80);             /* TSE 内部源（正式驱动同款） */
    cmd(0x04);
    wait_busy(5000);
    Serial.println("[ABT] init refresh...");
    cmd(0x12);
    wait_busy(10000);
    cmd(0x02);
    wait_busy(1000);
    Serial.println("[ABT] init done, sweep start");

    /* 扫档：X 从长到短，每档跑 2 轮（相位交替）观察累计与衰减 */
    for (size_t i = 0; i < sizeof(XMS) / sizeof(XMS[0]); i++) {
        for (int r = 0; r < 2; r++) {
            uint32_t ms = abort_round(r & 1, XMS[i]);
            Serial.printf("[ABT] X=%ums round%d took %ums\n",
                          (unsigned)XMS[i], r + 1, (unsigned)ms);
            delay(2500);             /* 停顿供肉眼观察 */
        }
    }

    /* 收尾：完整 0x12 跑通（settle + 清打断残影） */
    cmd(0x04);
    wait_busy(5000);
    fill_checkerboard(0);
    cmd(0x13);
    dat_burst(s_frame, sizeof(s_frame));
    cmd(0x12);
    wait_busy(10000);
    cmd(0x02);
    wait_busy(1000);
    Serial.println("[ABT] settle full refresh done, sweep complete");
}

void loop() { delay(1000); }
