/**
 * HINK-E0213A31-A0 (SSD1680) 强制全刷恢复探针
 * 
 * 目标：执行一次完整的全刷序列，恢复屏幕正常显示
 * 
 * 步骤：
 * 1. 硬件复位（清除所有寄存器状态）
 * 2. 完整初始化序列
 * 3. 写入全白帧
 * 4. 触发标准全刷（0xF7）
 */

#include <Arduino.h>
#include <SPI.h>

// 引脚配置
#define PIN_BUSY   12
#define PIN_RST    13
#define PIN_DC     9
#define PIN_CS     10

#define WIDTH  128
#define HEIGHT 250

static void delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

static void bus_cmd(uint8_t cmd) {
  digitalWrite(PIN_DC, LOW);
  digitalWrite(PIN_CS, LOW);
  SPI.transfer(cmd);
  digitalWrite(PIN_CS, HIGH);
}

static void bus_dat(uint8_t data) {
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  SPI.transfer(data);
  digitalWrite(PIN_CS, HIGH);
}

static bool wait_busy_low(uint32_t timeout_ms) {
  uint32_t start = millis();
  while (digitalRead(PIN_BUSY) == HIGH) {
    if (millis() - start > timeout_ms) return false;
    delay_ms(10);
  }
  return true;
}

void setup() {
  Serial.begin(115200);
  delay_ms(2000);
  Serial.println("\n=== SSD1680 强制全刷恢复 ===");
  
  pinMode(PIN_BUSY, INPUT);
  pinMode(PIN_RST, OUTPUT);
  pinMode(PIN_DC, OUTPUT);
  pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_CS, HIGH);
  
  SPI.begin(-1, -1, -1, PIN_CS);
  SPI.setFrequency(4000000);
  SPI.setBitOrder(MSBFIRST);
  SPI.setDataMode(SPI_MODE0);
  
  // 硬件复位
  Serial.println("1. 硬件复位...");
  digitalWrite(PIN_RST, HIGH); delay_ms(200);
  digitalWrite(PIN_RST, LOW);  delay_ms(10);
  digitalWrite(PIN_RST, HIGH); delay_ms(500);
  
  // SWRESET
  Serial.println("2. SWRESET...");
  bus_cmd(0x12);
  delay_ms(50);
  wait_busy_low(5000);
  
  // 初始化序列（与正式驱动相同）
  Serial.println("3. 初始化...");
  
  // Driver Output Control
  bus_cmd(0x01);
  bus_dat(0xF9); bus_dat(0x00); bus_dat(0x00);
  
  // Border
  bus_cmd(0x3C);
  bus_dat(0x05);
  
  // Display Update Control 2
  bus_cmd(0x21);
  bus_dat(0x00); bus_dat(0x80);
  
  // Temperature Sensor
  bus_cmd(0x18);
  bus_dat(0x80);
  
  // Data Entry Mode
  bus_cmd(0x11);
  bus_dat(0x03);
  
  // RAM Window
  bus_cmd(0x44);
  bus_dat(0x00); bus_dat(0x0F);
  bus_cmd(0x45);
  bus_dat(0x00); bus_dat(0x00);
  bus_dat(0xF9); bus_dat(0x00);
  
  // Address Counter
  bus_cmd(0x4E); bus_dat(0x00);
  bus_cmd(0x4F); bus_dat(0x00); bus_dat(0x00);
  
  Serial.println("4. 写入全白帧...");
  bus_cmd(0x26);
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  for (int i = 0; i < WIDTH * HEIGHT / 8; i++) {
    SPI.transfer(0xFF);
  }
  digitalWrite(PIN_CS, HIGH);
  
  bus_cmd(0x24);
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  for (int i = 0; i < WIDTH * HEIGHT / 8; i++) {
    SPI.transfer(0xFF);
  }
  digitalWrite(PIN_CS, HIGH);
  
  Serial.println("5. 触发全刷 (0xF7)...");
  bus_cmd(0x22);
  bus_dat(0xF7);
  bus_cmd(0x20);
  
  uint32_t start = millis();
  bool ok = wait_busy_low(10000);
  uint32_t elapsed = millis() - start;
  
  if (ok) {
    Serial.printf("全刷完成，耗时 %lu ms\n", elapsed);
  } else {
    Serial.printf("全刷超时，当前耗时 %lu ms\n", elapsed);
  }
  
  Serial.println("=== 恢复完成 ===");
}

void loop() {
  delay_ms(1000);
}
