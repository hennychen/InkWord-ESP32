/**
 * HINK-E0213A31-A0 (SSD1680) 自定义 LUT 局部刷新探针
 * 
 * 基于 Waveshare V3 驱动方案：
 * 1. 上传 153 字节自定义局部刷新波形到 0x32 寄存器
 * 2. 设置栅极电压（0x03）、源极电压（0x04）、VCOM（0x2C）
 * 3. 使用 0x0F 触发命令（不是 0xC7 或 0xF7）
 * 
 * 参考：https://dilder.dev/docs/tools/picotool-ota/
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

static void bus_dat_multi(const uint8_t* data, size_t len) {
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  for (size_t i = 0; i < len; i++) {
    SPI.transfer(data[i]);
  }
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

// Waveshare V3 局部刷新 LUT（159 字节）
// 来源：https://github.com/waveshare/e-Paper/blob/master/RaspberryPi_JetsonNano/c/lib/e-Paper/EPD_2in13_V3.c
// 实际 SSD1680 LUT 寄存器为 159 字节（LUT0-LUT4 + 栅极/源极/VCOM）
static const uint8_t WF_PARTIAL_2IN13_V3[159] = {
  0x0,0x40,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
  0x80,0x80,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
  0x40,0x40,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
  0x0,0x80,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
  0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
  0x14,0x0,0x0,0x0,0x0,0x0,0x0,  
  0x1,0x0,0x0,0x0,0x0,0x0,0x1,  
  0x0,0x0,0x0,0x0,0x0,0x0,0x0,
  0x0,0x0,0x0,0x0,0x0,0x0,0x0,
  0x0,0x0,0x0,0x0,0x0,0x0,0x0,
  0x0,0x0,0x0,0x0,0x0,0x0,0x0,
  0x0,0x0,0x0,0x0,0x0,0x0,0x0,
  0x0,0x0,0x0,0x0,0x0,0x0,0x0,
  0x0,0x0,0x0,0x0,0x0,0x0,0x0,
  0x0,0x0,0x0,0x0,0x0,0x0,0x0,
  0x0,0x0,0x0,0x0,0x0,0x0,0x0,
  0x0,0x0,0x0,0x0,0x0,0x0,0x0,
  0x22,0x22,0x22,0x22,0x22,0x22,0x0,0x0,0x0,
  0x22,0x17,0x41,0xB0,0x32,0x36,
};

// 栅极电压设置
static const uint8_t GATE_VOLTAGE[] = {0x03, 0x00};
// 源极电压设置  
static const uint8_t SOURCE_VOLTAGE[] = {0x04, 0x29, 0x29, 0x00};
// VCOM 设置
static const uint8_t VCOM[] = {0x2C, 0x17};

void setup() {
  Serial.begin(115200);
  delay_ms(2000);
  Serial.println("\n=== SSD1680 自定义 LUT 局部刷新探针 ===");
  
  pinMode(PIN_BUSY, INPUT);
  pinMode(PIN_RST, OUTPUT);
  pinMode(PIN_DC, OUTPUT);
  pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_CS, HIGH);
  
  SPI.begin(-1, -1, -1, PIN_CS);
  SPI.setFrequency(4000000);
  SPI.setBitOrder(MSBFIRST);
  SPI.setDataMode(SPI_MODE0);
  
  // 1. 硬件复位
  Serial.println("1. 硬件复位...");
  digitalWrite(PIN_RST, HIGH); delay_ms(200);
  digitalWrite(PIN_RST, LOW);  delay_ms(10);
  digitalWrite(PIN_RST, HIGH); delay_ms(500);
  wait_busy_low(5000);
  
  // 2. SWRESET
  Serial.println("2. SWRESET...");
  bus_cmd(0x12);
  delay_ms(50);
  wait_busy_low(5000);
  
  // 3. 初始化（与正式驱动相同）
  Serial.println("3. 初始化...");
  bus_cmd(0x01); bus_dat(0xF9); bus_dat(0x00); bus_dat(0x00);  // Driver Output
  bus_cmd(0x3C); bus_dat(0x05);  // Border
  bus_cmd(0x21); bus_dat(0x00); bus_dat(0x80);  // Display Update Control 2
  bus_cmd(0x18); bus_dat(0x80);  // Temperature Sensor
  bus_cmd(0x11); bus_dat(0x03);  // Data Entry Mode
  
  // RAM Window
  bus_cmd(0x44); bus_dat(0x00); bus_dat(0x0F);
  bus_cmd(0x45); bus_dat(0x00); bus_dat(0x00); bus_dat(0xF9); bus_dat(0x00);
  
  // 4. 加载自定义局部刷新 LUT（关键步骤！）
  Serial.println("4. 加载自定义局部刷新 LUT...");
  
  // 写入 LUT 到 0x32 寄存器（153 字节）
  bus_cmd(0x32);
  bus_dat_multi(WF_PARTIAL_2IN13_V3, sizeof(WF_PARTIAL_2IN13_V3));
  
  // 设置栅极电压
  bus_cmd(0x03);
  bus_dat(GATE_VOLTAGE[1]);
  
  // 设置源极电压
  bus_cmd(0x04);
  bus_dat(SOURCE_VOLTAGE[1]);
  bus_dat(SOURCE_VOLTAGE[2]);
  bus_dat(SOURCE_VOLTAGE[3]);
  
  // 设置 VCOM
  bus_cmd(0x2C);
  bus_dat(VCOM[1]);
  
  // 5. 启用 RAM ping-pong（0x37 bit 6）
  Serial.println("5. 启用 RAM ping-pong...");
  bus_cmd(0x37);
  bus_dat(0x40);  // bit 6 = 1
  
  // 6. 写入测试图像（全白）
  Serial.println("6. 写入测试图像...");
  bus_cmd(0x4E); bus_dat(0x00);
  bus_cmd(0x4F); bus_dat(0x00); bus_dat(0x00);
  
  bus_cmd(0x24);
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  for (int i = 0; i < WIDTH * HEIGHT / 8; i++) {
    SPI.transfer(0xFF);  // 全白
  }
  digitalWrite(PIN_CS, HIGH);
  
  // 7. 触发局部刷新（使用 0x0F！）
  Serial.println("7. 触发局部刷新 (0x0F)...");
  bus_cmd(0x0F);
  
  uint32_t start = millis();
  bool ok = wait_busy_low(10000);
  uint32_t elapsed = millis() - start;
  
  if (ok) {
    Serial.printf("局部刷新完成，耗时 %lu ms\n", elapsed);
  } else {
    Serial.printf("局部刷新超时，当前耗时 %lu ms\n", elapsed);
  }
  
  Serial.println("=== 测试完成 ===");
}

void loop() {
  delay_ms(1000);
}
