/* probe_ssd16_fprint.cpp —— SSD16xx 族指纹实测探针（4.26" GDEQ0426T82）
 *
 * 轮次记录：
 *   v1：低电平期采样，0x2F 读回 FF；0x44 读 00 / 0x45 读 FF 稳定分化；
 *   v2：写窗口(0x44=00 00 1F 03, 0x45=00 00 DF 01)后 0x2F rise 读回 01
 *     ×4（唯一一次非残留读回，与 datasheet POR=0x01 吻合）；0x44/0x45
 *     读回 FE/FD 不跟随窗口写入；
 *   v3：纯 RST（零写入）下 delay×边沿全扫描 0x2F 均 FF —— v2 的 01
 *     无法复现 → 01 的必要条件疑似「先写 0x44/0x45 窗口」。另发现：
 *     ①读回残留规律：读回值=所发命令 bit0（COG 读回全程无驱动，
 *     0x00/22/24/44/50→00，0x11/21/27/2F/45/61/7F→FF）；
 *     ②RST 后 BUSY 三轮 5s 无忙窗（SSD1677 自检不拉 BUSY）——生产
 *     auto-detect 证据①「忙闲往返判活」对本屏恒 false（probe 必 -1）。
 *
 * v4 实验矩阵（精确复现 v2 T12 并拆解激活条件，每轮独立 RST）：
 *   R1  复现：写 0x44+0x45 全参数 → delay(1) → 读 0x2F ×2（rise）
 *   R2  拆分：只写 0x44（全参数）→ 读 0x2F
 *   R3  拆分：只写 0x45（全参数）→ 读 0x2F
 *   R4  只写 0x44 命令字节（无参数）→ 读 0x2F
 *   R5  若 R1 复现：读回模式激活期间连读 0x2F×8 / 0x44×4 / 0x45×4 /
 *       0x4E×4 / 0x27×6 —— 找窗口真值流（分辨率路径复活判定）
 *   R6  写不同窗口值（0x44 XEA=0x000/0x3FF）→ 读 0x2F —— 01 是否
 *       随窗口值变（残留回显 vs 寄存器读出判别）
 *   R7  对照：写 0x44 后读 0x2F 用 low 采样 —— v2 的 00（wr low→00）
 *
 * 纯数字操作：无升压刷屏；RST 复位不清屏；寄存器写入每轮 RST 恢复。
 * 引脚 = gpio_config.h EPD_*（SCK7/MOSI8/DC9/CS10/BUSY12/RST13）。
 */
#include <Arduino.h>

static const int PIN_SCK = 7, PIN_MOSI = 8, PIN_DC = 9,
                 PIN_CS = 10, PIN_BUSY = 12, PIN_RST = 13;

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

/* 上升沿采样 */
static uint8_t bb_byte_in_rise(void)
{
    uint8_t v = 0;
    for (int i = 7; i >= 0; i--) {
        digitalWrite(PIN_SCK, HIGH);
        delayMicroseconds(2);
        v = (uint8_t)((v << 1) | (digitalRead(PIN_MOSI) ? 1 : 0));
        delayMicroseconds(2);
        digitalWrite(PIN_SCK, LOW);
        delayMicroseconds(2);
    }
    return v;
}

/* 低电平期采样 */
static uint8_t bb_byte_in_low(void)
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

/* 单事务连读：命令 → 数据相位 → delay_ms → 连读 n 字节（rise/low） */
static void read_txn(uint8_t c, uint8_t *buf, int n, uint32_t delay_ms,
                     bool rise)
{
    bb_mode_write();
    digitalWrite(PIN_CS, LOW);
    digitalWrite(PIN_DC, LOW);
    bb_byte_out(c);
    digitalWrite(PIN_DC, HIGH);
    bb_mode_read();
    if (delay_ms) delay(delay_ms);
    for (int i = 0; i < n; i++)
        buf[i] = rise ? bb_byte_in_rise() : bb_byte_in_low();
    digitalWrite(PIN_CS, HIGH);
    bb_mode_write();
}

/* RST 复位 + 等 BUSY（SSD16xx LOW-idle，本屏无忙窗直接过）+ 静置 */
static void cog_reset(void)
{
    digitalWrite(PIN_RST, HIGH); delay(10);
    digitalWrite(PIN_RST, LOW);  delay(10);
    digitalWrite(PIN_RST, HIGH); delay(10);
    delay(5);
    uint32_t t0 = millis();
    while (digitalRead(PIN_BUSY) == HIGH) {   /* 万一有忙窗 */
        if (millis() - t0 > 5000) break;
        delay(1);
    }
    delay(300);
}

static void dump(const char *tag, const uint8_t *buf, int n)
{
    char line[112];
    int p = snprintf(line, sizeof(line), "%-24s", tag);
    for (int i = 0; i < n; i++)
        p += snprintf(line + p, sizeof(line) - p, " %02X", buf[i]);
    Serial.println(line);
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

    uint8_t b[8];

    /* R1：精确复现 v2 T11+T12（写双窗口 → delay1 → 读 0x2F rise） */
    cog_reset();
    cmd(0x44); dat(0x00); dat(0x00); dat(0x1F); dat(0x03);
    cmd(0x45); dat(0x00); dat(0x00); dat(0xDF); dat(0x01);
    delay(1);
    read_txn(0x2F, b, 2, 0, true);   /* delay 0：v2 T12 的 delay(1) 已在上 */
    dump("R1 0x2F wr-both rise:", b, 2);
    read_txn(0x2F, b, 2, 1, true);
    dump("R1 0x2F wr-both d1:", b, 2);

    /* R2：只写 0x44（全参数） */
    cog_reset();
    cmd(0x44); dat(0x00); dat(0x00); dat(0x1F); dat(0x03);
    delay(1);
    read_txn(0x2F, b, 2, 0, true);
    dump("R2 0x2F wr-44 rise:", b, 2);

    /* R3：只写 0x45（全参数） */
    cog_reset();
    cmd(0x45); dat(0x00); dat(0x00); dat(0xDF); dat(0x01);
    delay(1);
    read_txn(0x2F, b, 2, 0, true);
    dump("R3 0x2F wr-45 rise:", b, 2);

    /* R4：只写 0x44 命令字节（无参数） */
    cog_reset();
    cmd(0x44);
    delay(1);
    read_txn(0x2F, b, 2, 0, true);
    dump("R4 0x2F wr-44c rise:", b, 2);

    /* R5：若读回模式激活——连读各命令找窗口真值流 */
    cog_reset();
    cmd(0x44); dat(0x00); dat(0x00); dat(0x1F); dat(0x03);
    cmd(0x45); dat(0x00); dat(0x00); dat(0xDF); dat(0x01);
    delay(1);
    read_txn(0x2F, b, 8, 0, true);    dump("R5 0x2F x8:", b, 8);
    read_txn(0x44, b, 4, 0, true);    dump("R5 0x44 x4:", b, 4);
    read_txn(0x45, b, 4, 0, true);    dump("R5 0x45 x4:", b, 4);
    read_txn(0x4E, b, 4, 0, true);    dump("R5 0x4E x4:", b, 4);
    read_txn(0x4F, b, 4, 0, true);    dump("R5 0x4F x4:", b, 4);
    read_txn(0x27, b, 6, 0, true);    dump("R5 0x27 x6:", b, 6);

    /* R6：不同窗口值 → 0x2F 是否随变（XEA=0x3FF=datasheet POR） */
    cog_reset();
    cmd(0x44); dat(0x00); dat(0x00); dat(0xFF); dat(0x03);
    cmd(0x45); dat(0x00); dat(0x00); dat(0xA7); dat(0x02);
    delay(1);
    read_txn(0x2F, b, 2, 0, true);
    dump("R6 0x2F wr-3FF rise:", b, 2);
    cog_reset();
    cmd(0x44); dat(0x00); dat(0x00); dat(0x00); dat(0x00);
    cmd(0x45); dat(0x00); dat(0x00); dat(0x00); dat(0x00);
    delay(1);
    read_txn(0x2F, b, 2, 0, true);
    dump("R6 0x2F wr-000 rise:", b, 2);

    /* R7：写窗口后 0x2F low 采样对照 */
    cog_reset();
    cmd(0x44); dat(0x00); dat(0x00); dat(0x1F); dat(0x03);
    cmd(0x45); dat(0x00); dat(0x00); dat(0xDF); dat(0x01);
    delay(1);
    read_txn(0x2F, b, 2, 0, false);
    dump("R7 0x2F wr-both low:", b, 2);

    Serial.println("[SSD16-FP] v4 done");
}

void loop() { delay(1000); }
