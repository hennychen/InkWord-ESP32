/* probe_opm_tsw.cpp —— OPM021EB 温度档扫描探针（三十轮）
 *
 * 背景（probe_opm_otp dump 实证）：OTP 烧了 6 组不同波形，时间参数
 * 递减（电平对 0x46→0x23→0x19→0x14 组），温度断点表 0x002~0x007 =
 * D7/00/17/17/3E/00（-41/0/23/23/62°C）。室温 ~25°C 落 (23,62] 档；
 * 断点 0x3E(+62) 之后映射尾部 0x14 电平组（最快候选）。
 *
 * 注入路径（UC8151D datasheet）：
 *   TSE  (0x1C)=0x80  禁内部传感器（外挂 LM75 已裁，防悬空干扰）
 *   CCSET(0xE0)=0x02  TSFIX=1：温度由 TS_SET 寄存器决定
 *   TSSET(0xE5)=t     直写温度（8 位补码）
 *   datasheet 流程图：每次 DRF(0x12) 按当前温度重新选档读 LUT——
 *   改 TSSET 即时生效，无需 RST。
 *
 * 流程：完整 init（照 panel_opm021eb uc_init，含首次 0x12 初始化刷）
 * → 扫 10 个温度档，每档：TSSET → 0x13 棋盘帧（相位交替）→ 0x12
 * 计时 BUSY。全程 bit-bang（与 probe_opm_otp 同基建）。
 * 观察要点：各档刷新时长差 + 棋盘黑白块对比度（高温档驱动短，
 * 黑度可能略浅为预期）。
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

static uint32_t measure_refresh(void)
{
    uint32_t t0 = millis();
    cmd(0x12);
    wait_busy(10000);
    return millis() - t0;
}

/* 棋盘帧：8x8 像素块交替（16B/行 x 250 行，122 宽第 16 字节 padding） */
static uint8_t s_frame[16 * 250];

static void fill_checkerboard(int phase)
{
    for (int y = 0; y < 250; y++)
        for (int b = 0; b < 16; b++)
            s_frame[y * 16 + b] =
                (uint8_t)((((b ^ (y >> 3)) & 1) ^ phase) ? 0xFF : 0x00);
}

static const int8_t TEMPS[] = {
    30, 40, 50, 62, 70, 85, 100, 0, -15, -41,
};

void setup()
{
    Serial.begin(115200);
    delay(300);
    bb_init_pins();

    /* RST（照正式驱动 uc_init 时序） */
    digitalWrite(PIN_RST, HIGH); delay(200);
    digitalWrite(PIN_RST, LOW);  delay(10);
    digitalWrite(PIN_RST, HIGH); delay(10);
    wait_busy(5000);
    Serial.println("[TSW-PROBE] COG reset ok");

    cmd(0x00); dat(0x1F);                        /* PSR：OTP 模式（单字节） */
    cmd(0x61); dat(122); dat(0); dat(250);       /* TRES：122 x 250 */
    cmd(0x50); dat(0x97);                        /* CDI（正式驱动同款） */

    /* 温度注入三件套（首次 0x12 初始化刷之前，温度表加载即用注入值） */
    cmd(0x1C); dat(0x80);                        /* TSE：禁内部传感器 */
    cmd(0xE0); dat(0x02);                        /* CCSET：TSFIX=1 */
    cmd(0xE5); dat((uint8_t)25);                 /* TSSET：先 +25C 基准 */

    /* booster on + 首次 0x12 初始化刷（十三轮：RST 后首刷完成内部
     * 初始化，之前 RAM 写入无效；此处温度 25C = 室温基准档） */
    Serial.println("[TSW-PROBE] init refresh (+25C baseline)...");
    cmd(0x04);
    wait_busy(5000);
    {
        uint32_t ms = measure_refresh();
        Serial.printf("[TSW-PROBE] init 0x12 took %ums\n", (unsigned)ms);
    }
    cmd(0x02);
    wait_busy(1000);

    /* 扫档主循环 */
    for (size_t i = 0; i < sizeof(TEMPS); i++) {
        int8_t t = TEMPS[i];
        cmd(0x04);
        wait_busy(5000);
        cmd(0xE5); dat((uint8_t)t);              /* TSSET：切温度档 */
        fill_checkerboard((int)(i & 1));         /* 相位交替（可视验证） */
        cmd(0x13);                               /* DTM2 新帧 */
        dat_burst(s_frame, sizeof(s_frame));
        uint32_t ms = measure_refresh();
        Serial.printf("[TSW] temp=%+dC(0x%02X) phase=%u busy=%ums\n",
                      (int)t, (uint8_t)t, (unsigned)(i & 1), (unsigned)ms);
        cmd(0x02);
        wait_busy(1000);
        delay(3000);                             /* 停顿供肉眼观察棋盘 */
    }
    Serial.println("[TSW-PROBE] sweep done");
}

void loop() { delay(1000); }
