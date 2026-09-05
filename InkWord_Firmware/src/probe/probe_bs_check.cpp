/* probe_bs_check.cpp —— BS 引脚状态检查
 *
 * 目的：确认 BS 引脚是否为 LOW（4 线 SPI 模式）
 * 方法：读取 GPIO11（原 BS 引脚，现 ES8311 DOUT）电平
 *       如果 BS 未接 LOW，SSD1680 会进入 3 线 SPI 模式
 *
 * 用法：pio run -e bs-check -t upload && pio device monitor -b 115200
 */
#include <Arduino.h>

/* 原 BS 引脚位置（J2-10） */
static const int PIN_BS_LOCATION = 11;  /* GPIO11，现用于 ES8311 DOUT */

void setup()
{
    Serial.begin(115200);
    delay(300);

    Serial.println("=== BS Pin Check ===");
    Serial.println("Checking J2-10 (original BS pin location)...");

    /* 将 GPIO11 设为输入，读取外部电平 */
    pinMode(PIN_BS_LOCATION, INPUT);
    delay(100);

    int bs_level = digitalRead(PIN_BS_LOCATION);
    Serial.printf("BS pin (J2-10) level: %s\n",
                  bs_level ? "HIGH (3-line SPI!)" : "LOW (4-line SPI OK)");

    if (bs_level == HIGH) {
        Serial.println("\n*** WARNING ***");
        Serial.println("BS is HIGH! SSD1680 will use 3-line SPI mode.");
        Serial.println("This causes communication failure.");
        Serial.println("\nFix: Short J2-10 to GND on the adapter board.");
    } else {
        Serial.println("\nBS is LOW. 4-line SPI mode confirmed.");
        Serial.println("If screen still fails, check:");
        Serial.println("  - FPC insertion direction");
        Serial.println("  - VCI 3.3V supply");
        Serial.println("  - FPC contact quality");
    }

    Serial.println("\n=== Loop ===");
}

void loop()
{
    static uint32_t last = 0;
    if (millis() - last > 2000) {
        int bs = digitalRead(PIN_BS_LOCATION);
        Serial.printf("BS=%s\n", bs ? "HIGH" : "LOW");
        last = millis();
    }
    delay(100);
}
