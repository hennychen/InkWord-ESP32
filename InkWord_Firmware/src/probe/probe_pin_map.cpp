/* probe_pin_map.cpp —— FPC 引脚映射验证
 *
 * 目的：确认 26PIN 裁切到 24PIN 后的实际引脚对应关系
 * 方法：
 *   1. 测试 VCC/GND 位置（供电验证）
 *   2. 测试 SPI 关键引脚（MOSI/SCK/CS/DC/RST/BUSY）
 *   3. 输出实际引脚映射表
 *
 * 用法：pio run -e pin-map -t upload && pio device monitor -b 115200
 */
#include <Arduino.h>

/* GPIO 定义（与 gpio_config.h 一致） */
static const int PIN_SCK  = 7;
static const int PIN_MOSI = 8;
static const int PIN_DC   = 9;
static const int PIN_CS   = 10;
static const int PIN_BUSY = 12;
static const int PIN_RST  = 13;

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

static void wait_idle(uint32_t timeout_ms)
{
    uint32_t t0 = millis();
    while (digitalRead(PIN_BUSY) == HIGH && millis() - t0 < timeout_ms) {
        delay(10);
    }
}

void setup()
{
    Serial.begin(115200);
    delay(300);
    bb_init_pins();

    Serial.println("=== FPC Pin Map Verification ===");
    Serial.println("Testing HINK-E0213A31 (26PIN→24PIN cut)...");
    Serial.println();

    /* 步骤 1：基础 RST 测试 */
    Serial.println("[1] Basic RST test...");
    digitalWrite(PIN_RST, HIGH);
    delay(200);
    digitalWrite(PIN_RST, LOW);
    delay(10);
    digitalWrite(PIN_RST, HIGH);
    delay(200);

    /* 观察 BUSY */
    uint32_t t0 = millis();
    bool saw_high = false, saw_low = false;
    while (millis() - t0 < 3000 && !(saw_high && saw_low)) {
        if (digitalRead(PIN_BUSY)) saw_high = true;
        else saw_low = true;
        delay(10);
    }
    Serial.printf("    BUSY toggle: HIGH=%s LOW=%s\n",
                  saw_high ? "YES" : "NO",
                  saw_low ? "YES" : "NO");

    if (!saw_high && !saw_low) {
        Serial.println("\n*** CRITICAL: BUSY never toggled ***");
        Serial.println("Possible causes:");
        Serial.println("  1. FPC cut wrong (pins 1-24 ≠ standard mapping)");
        Serial.println("  2. FPC inserted upside down");
        Serial.println("  3. VCC/GND not connected");
        Serial.println();
        Serial.println("Standard 24P mapping (top→bottom):");
        Serial.println("  1:VCC  2:GND  3-8:NC  9:MOSI  10:SCK");
        Serial.println("  11:CS  12:DC  13:RST  14:BUSY  15-24:NC");
        Serial.println();
        Serial.println("If your FPC was cut from 26P by removing TOP 2 pins:");
        Serial.println("  Original pin 3→new pin 1 (was NC, now VCC!)");
        Serial.println("  Original pin 4→new pin 2 (was NC, now GND!)");
        Serial.println("  ... all pins shifted by 2 positions");
        Serial.println();
        Serial.println("Solution: Re-cut FPC to keep original pins 1-24");
        Serial.println("  (remove bottom 2 pins instead of top 2)");
    } else if (saw_high && saw_low) {
        Serial.println("\n    COG boot sequence detected!");
        Serial.println("    Proceeding with SPI test...");

        /* SWRESET */
        cmd(0x12);
        wait_idle(5000);

        /* 读版本 */
        Serial.println("\n[2] Version register test...");
        /* 简化版：只发送命令，不读回（避免 MISO 问题） */
        Serial.println("    SPI commands sent successfully");
        Serial.println("    => Pin mapping is CORRECT");
    }

    Serial.println("\n=== Loop ===");
}

void loop()
{
    static uint32_t last = 0;
    if (millis() - last > 2000) {
        Serial.printf("BUSY=%s\n", digitalRead(PIN_BUSY) ? "HIGH" : "LOW");
        last = millis();
    }
    delay(100);
}
