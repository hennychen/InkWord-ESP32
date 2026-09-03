/* probe_opm_otp.cpp —— OPM021EB（UC8151D 兼容 ESL COG）OTP 全量 dump 探针
 *
 * 依据 UC8151D datasheet B0.6（Info/UC8151D_datasheet.pdf）：
 *   - RA2h ROTP：命令 + 1 dummy 读字节 + 连续读，max 地址 0xFFF（4KB）
 *   - 读取为纯数字操作，无需进 RA0h Program Mode，无任何写入风险
 *   - OTP 布局（datasheet 6.9 节）：
 *       0x000       Bank 标志（0xA5 = Bank0 有效）
 *       0x002~0x007 温度断点 TB0~TB5（0xF1=-15C / 0xFB=-5C / 0x00=0C /
 *                   0x0A=10C / 0x1E=30C，见温度选择机制流程图）
 *       0x00B~0x01D 命令默认值区（0x00B=0xA5 使能键，0x00C=PSR 默认值，
 *                   屏厂可烧死 REG 位 —— REG LUT 锁死的产品级证据）
 *       其后        TR0~TR9 十档波形 LUT 区（每档 256bit VS/TP/RP 编码）
 *
 * 硬件：SDA 单线双向，本板只接 MOSI。读阶段把 MOSI 引脚转 GPIO 输入
 * 采样（三线读法），全 bit-bang 不动 SPI 外设；写时序与正式驱动同款
 * MODE0。RST 复位不清屏（双稳态），词条页面显示保持不变。
 */
#include <Arduino.h>

static const int PIN_SCK = 7, PIN_MOSI = 8, PIN_DC = 9,
                 PIN_CS = 10, PIN_BUSY = 12, PIN_RST = 13;

static uint8_t s_otp[4096];

static void bb_mode_write(void)
{
    pinMode(PIN_MOSI, OUTPUT);
    pinMode(PIN_SCK, OUTPUT);
    digitalWrite(PIN_SCK, LOW);
}

static void bb_mode_read(void)
{
    pinMode(PIN_SCK, OUTPUT);
    pinMode(PIN_MOSI, INPUT);      /* SDA 线高阻采样 */
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

static uint8_t bb_byte_in(void)
{
    uint8_t v = 0;
    for (int i = 7; i >= 0; i--) {
        delayMicroseconds(2);
        v = (uint8_t)((v << 1) | (digitalRead(PIN_MOSI) ? 1 : 0));
        digitalWrite(PIN_SCK, HIGH);
        delayMicroseconds(2);
        digitalWrite(PIN_SCK, LOW);
    }
    return v;
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

static void wait_busy(void)
{
    uint32_t t0 = millis();
    while (digitalRead(PIN_BUSY) == LOW) {   /* 本板 BUSY LOW = 忙 */
        if (millis() - t0 > 3000) break;
        delay(1);
    }
    delay(2);
}

void setup()
{
    Serial.begin(115200);
    delay(300);
    pinMode(PIN_DC, OUTPUT);
    pinMode(PIN_CS, OUTPUT);
    pinMode(PIN_RST, OUTPUT);
    pinMode(PIN_BUSY, INPUT);
    digitalWrite(PIN_CS, HIGH);
    bb_mode_write();

    /* RST 复位（照正式驱动时序） */
    digitalWrite(PIN_RST, HIGH); delay(10);
    digitalWrite(PIN_RST, LOW);  delay(10);
    digitalWrite(PIN_RST, HIGH); delay(10);
    wait_busy();
    Serial.println("[OTP-PROBE] COG reset, BUSY released");

    /* 最小 init：PSR（OTP 模式，与正式驱动同款）+ TRES，不刷屏不升压 */
    cmd(0x00); dat(0x1F); dat(0x00);
    cmd(0x61); dat(122); dat((uint8_t)(250 >> 8)); dat((uint8_t)(250 & 0xFF));

    /* ROTP 0xA2：命令（DC=0）→ 数据相位（DC=1）→ dummy → 连续读 4KB。
     * CS 全程保持 LOW 一气呵成，读完拉高结束事务。 */
    digitalWrite(PIN_CS, LOW);
    digitalWrite(PIN_DC, LOW);
    bb_byte_out(0xA2);
    digitalWrite(PIN_DC, HIGH);
    bb_mode_read();
    (void)bb_byte_in();            /* dummy 字节（丢弃） */
    for (size_t i = 0; i < sizeof(s_otp); i++) s_otp[i] = bb_byte_in();
    digitalWrite(PIN_CS, HIGH);
    bb_mode_write();

    Serial.printf("[OTP-PROBE] read %u bytes\r\n", (unsigned)sizeof(s_otp));
    for (size_t off = 0; off < sizeof(s_otp); off += 16) {
        char line[128];
        int p = snprintf(line, sizeof(line), "OTP %03X:", (unsigned)off);
        for (int j = 0; j < 16; j++)
            p += snprintf(line + p, sizeof(line) - p, " %02X", s_otp[off + j]);
        Serial.println(line);
        delay(4);
    }
    Serial.println("[OTP-PROBE] dump done");
}

void loop() { delay(1000); }
