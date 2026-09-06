/* RST 枚举 × BUSY 全扫 v3 = v2 + 板载 WS2812(GPIO48) 结果指示
 * 绿灯常亮 = 找到 RST/BUSY 走线（插对了！）
 * 红灯闪 3 下 = 本轮零命中（换插法，几秒后自动重扫）
 * 物理试插循环：拔屏 → 换分区/偏移/翻面 → 看灯。 */
#include <Arduino.h>

static const uint8_t CAND[] = {
    1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 21, 33, 34, 38, 39, 40, 41, 42, 47, 48
};
#define NCAND (int)(sizeof(CAND) / sizeof(CAND[0]))

#ifndef NEOPIXEL_PIN
#define NEOPIXEL_PIN 48
#endif

void setup() {
  Serial.begin(115200);
  delay(1500);
  neopixelWrite(NEOPIXEL_PIN, 0, 0, 0);
}

int probe_one_rst(uint8_t rst) {
  int busy = -1;
  for (int i = 0; i < NCAND; i++) {
    if (CAND[i] == rst) continue;
    pinMode(CAND[i], INPUT_PULLDOWN);
  }
  delay(3);
  uint8_t init_high[NCAND];
  for (int i = 0; i < NCAND; i++)
    init_high[i] = (CAND[i] == rst) ? 0 : (digitalRead(CAND[i]) == 1);

  pinMode(rst, OUTPUT);
  digitalWrite(rst, HIGH);
  delay(2);
  digitalWrite(rst, LOW);
  delay(10);
  digitalWrite(rst, HIGH);

  int hit_idx = -1, hit_t = -1;
  for (int t = 0; t < 1500 && hit_idx < 0; t++) {
    for (int i = 0; i < NCAND; i++) {
      if (CAND[i] == rst || init_high[i]) continue;
      if (digitalRead(CAND[i]) == 1) { hit_idx = i; hit_t = t; break; }
    }
    delayMicroseconds(200);
  }
  if (hit_idx >= 0) {
    int high_cnt = 0;
    for (int k = 0; k < 3; k++) {
      if (digitalRead(CAND[hit_idx]) == 1) high_cnt++;
      delay(1);
    }
    if (high_cnt >= 2) {
      busy = CAND[hit_idx];
      Serial.printf("  RST=GPIO%u -> BUSY=GPIO%u  <<< 命中！(释放后 %d*0.2ms)\n",
                    (unsigned)rst, (unsigned)busy, hit_t);
    }
  }
  pinMode(rst, INPUT_PULLDOWN);
  return busy;
}

void loop() {
  Serial.println("\n=== RST枚举 x BUSY全扫 v3（绿=命中 红闪=不通）===");
  int hit_rst = -1, hit_busy = -1;
  for (int r = 0; r < NCAND; r++) {
    int b = probe_one_rst(CAND[r]);
    if (b >= 0) { hit_rst = CAND[r]; hit_busy = b; }
    delay(20);
  }
  if (hit_rst >= 0) {
    neopixelWrite(NEOPIXEL_PIN, 0, 255, 0);        /* 绿：命中 */
    Serial.printf(">>> 命中 RST=GPIO%d BUSY=GPIO%d，保持插法别动！\n",
                  hit_rst, hit_busy);
    Serial.println(">>> 把 (RST,BUSY) 脚号告诉我，据此修正全组引脚后即可点亮。");
    delay(10000);
    neopixelWrite(NEOPIXEL_PIN, 0, 0, 0);
  } else {
    for (int k = 0; k < 3; k++) {                   /* 红闪 3 下：不通 */
      neopixelWrite(NEOPIXEL_PIN, 255, 0, 0); delay(250);
      neopixelWrite(NEOPIXEL_PIN, 0, 0, 0);     delay(250);
    }
    Serial.println(">>> 零命中。换一种插法（分区/偏移/翻面），自动重扫中...");
  }
  delay(1000);
}
