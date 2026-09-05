/**
 * 按键硬件测试探针
 * 
 * 目标：验证五向导航按键硬件是否正常工作
 * 
 * 测试逻辑：
 * 1. 配置所有按键引脚为上拉输入
 * 2. 循环检测每个按键状态
 * 3. 按下时打印按键名称
 */

#include <Arduino.h>

// 按键引脚定义（与 gpio_config.h 一致）
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

const int button_count = sizeof(button_pins) / sizeof(button_pins[0]);

// 记录上次状态，避免重复打印
bool last_state[7] = {false};

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== 按键硬件测试 ===");
  
  // 配置所有按键引脚为上拉输入
  for (int i = 0; i < button_count; i++) {
    pinMode(button_pins[i], INPUT_PULLUP);
    Serial.printf("按键 %s 配置在 GPIO%d\n", button_names[i], button_pins[i]);
  }
  
  Serial.println("\n请按下任意按键测试...");
}

void loop() {
  for (int i = 0; i < button_count; i++) {
    // 低电平有效（上拉，按下接地）
    bool pressed = (digitalRead(button_pins[i]) == LOW);
    
    // 状态变化时打印
    if (pressed != last_state[i]) {
      last_state[i] = pressed;
      if (pressed) {
        Serial.printf("[按下] %s (GPIO%d)\n", button_names[i], button_pins[i]);
      } else {
        Serial.printf("[释放] %s (GPIO%d)\n", button_names[i], button_pins[i]);
      }
    }
  }
  
  delay(50);  // 简单去抖
}
