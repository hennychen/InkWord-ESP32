/**
 * GPIO 电平扫描探针
 * 
 * 目标：检测所有按键引脚的原始电平，判断硬件状态
 */

#include <Arduino.h>

#define NAV_UP_PIN      1
#define NAV_DOWN_PIN    2
#define NAV_LEFT_PIN    14
#define NAV_RIGHT_PIN   15
#define NAV_CENTER_PIN  21
#define NAV_SET_PIN     42
#define NAV_RST_PIN     40

const int button_pins[] = {
  NAV_UP_PIN, NAV_DOWN_PIN, NAV_LEFT_PIN, NAV_RIGHT_PIN, 
  NAV_CENTER_PIN, NAV_SET_PIN, NAV_RST_PIN
};
const char* button_names[] = {
  "UP", "DOWN", "LEFT", "RIGHT", "CENTER", "SET", "RST"
};
const int button_count = 7;

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== GPIO 电平扫描 ===");
  
  // 先以上拉输入模式读取
  Serial.println("\n--- 上拉输入模式 ---");
  for (int i = 0; i < button_count; i++) {
    pinMode(button_pins[i], INPUT_PULLUP);
    delay(10);
    int level = digitalRead(button_pins[i]);
    Serial.printf("GPIO%d (%s): %s\n", 
      button_pins[i], button_names[i], level ? "HIGH" : "LOW");
  }
  
  // 再以浮空输入模式读取（不启用内部上拉）
  Serial.println("\n--- 浮空输入模式 ---");
  for (int i = 0; i < button_count; i++) {
    pinMode(button_pins[i], INPUT);
    delay(10);
    int level = digitalRead(button_pins[i]);
    Serial.printf("GPIO%d (%s): %s\n", 
      button_pins[i], button_names[i], level ? "HIGH" : "LOW");
  }
  
  // 持续扫描，检测电平变化
  Serial.println("\n--- 持续扫描（请按键）---");
  for (int i = 0; i < button_count; i++) {
    pinMode(button_pins[i], INPUT_PULLUP);
  }
  
  uint32_t start = millis();
  while (millis() - start < 15000) {
    for (int i = 0; i < button_count; i++) {
      int level = digitalRead(button_pins[i]);
      if (level == LOW) {
        Serial.printf("[%lu ms] GPIO%d (%s) = LOW (按下)\n", 
          (unsigned long)(millis() - start), button_pins[i], button_names[i]);
        delay(100);  // 去抖
      }
    }
    delay(20);
  }
  
  Serial.println("\n=== 扫描完成 ===");
}

void loop() {
  delay(1000);
}
