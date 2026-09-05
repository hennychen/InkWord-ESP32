/**
 * @file probe_310_busy.cpp
 * @brief 3.1" 屏 BUSY 引脚诊断 —— 检测 COG 响应与 BUSY 极性
 */
#include <Arduino.h>
#include "../gpio_config.h"

void setup() {
    Serial.begin(115200);
    delay(3000);
    Serial.println("\n=== 3.1\" UC8253 BUSY Pin Diagnostic ===\n");

    pinMode(EPD_BUSY_PIN, INPUT);
    pinMode(EPD_RESET_PIN, OUTPUT);
    pinMode(EPD_CS_PIN, OUTPUT);
    pinMode(EPD_DC_PIN, OUTPUT);
    digitalWrite(EPD_CS_PIN, HIGH);
    digitalWrite(EPD_DC_PIN, LOW);

    // 1. Sample BUSY before reset
    int busy_before = digitalRead(EPD_BUSY_PIN);
    Serial.printf("[1] BUSY before reset: %s\n", busy_before ? "HIGH" : "LOW");

    // 2. Hardware reset
    Serial.println("\n[2] Performing HW reset (RST HIGH->LOW->HIGH)...");
    digitalWrite(EPD_RESET_PIN, HIGH);
    delay(200);
    digitalWrite(EPD_RESET_PIN, LOW);
    delay(20);  // 20ms pulse
    digitalWrite(EPD_RESET_PIN, HIGH);

    // 3. Monitor BUSY for 5 seconds after reset
    Serial.println("\n[3] Monitoring BUSY for 5s after reset:");
    uint32_t t0 = millis();
    bool saw_high = false, saw_low = false;
    int last_val = -1;
    int transitions = 0;

    while (millis() - t0 < 5000) {
        int val = digitalRead(EPD_BUSY_PIN);
        if (val) saw_high = true; else saw_low = true;
        if (val != last_val && last_val != -1) {
            transitions++;
            Serial.printf("    t=%4ums: BUSY -> %s (transition #%d)\n",
                          (unsigned)(millis() - t0), val ? "HIGH" : "LOW", transitions);
        }
        last_val = val;
        delay(50);
    }

    // 4. Summary
    Serial.println("\n[4] Summary:");
    Serial.printf("    Saw HIGH: %s\n", saw_high ? "YES" : "NO");
    Serial.printf("    Saw LOW:  %s\n", saw_low ? "YES" : "NO");
    Serial.printf("    Transitions: %d\n", transitions);

    // 5. Final idle level (sample 5 times)
    delay(500);
    int high_cnt = 0;
    for (int i = 0; i < 5; i++) {
        if (digitalRead(EPD_BUSY_PIN)) high_cnt++;
        delay(50);
    }
    Serial.printf("\n[5] Idle level (5 samples): %d HIGH, %d LOW\n", high_cnt, 5 - high_cnt);

    // 6. Diagnosis
    Serial.println("\n[6] Diagnosis:");
    if (!saw_high && !saw_low) {
        Serial.println("    ERROR: No BUSY activity - check wiring!");
    } else if (saw_high && saw_low) {
        Serial.println("    COG alive! BUSY transitions detected.");
        if (high_cnt >= 4) {
            Serial.println("    -> Idle=HIGH, busy_level should be 1 (SSD16xx traits)");
        } else if (high_cnt <= 1) {
            Serial.println("    -> Idle=LOW, busy_level should be 0 (UC8253 traits)");
        } else {
            Serial.println("    -> Ambiguous idle level, check again");
        }
    } else if (saw_high && !saw_low) {
        Serial.println("    BUSY stuck HIGH - COG may not be responding");
        Serial.println("    -> Check FPC seating, SPI wiring, VCI 3.3V");
    } else {
        Serial.println("    BUSY stuck LOW - unusual, check wiring");
    }

    Serial.println("\n=== Diagnostic complete ===");
}

void loop() {
    delay(1000);
}
