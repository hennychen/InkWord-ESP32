/**
 * @file probe_audio_volume_sweep.cpp
 * @brief 喇叭响度 ↔ 音量档扫描探针（env:audio-volume-sweep，2026-09-19）
 *
 * 起因：3.1" 机上人耳报「喇叭接近没声音」。主机侧读 NVS 实态
 * （esptool read_flash 0x9000 + 条目解码）：`set_vol` = 25/100，写轨迹
 * 65→55→45→35→25（菜单每档 ±10），而默认档 75。音量映射是
 * `REG32 = vol*255/100`，25 档即 0x3F（75 档 0xBF）——codec 数字音量
 * 对数感知下低档衰减极快，25 档本就接近静音。
 *
 * 本探针免按键、免菜单、免改 NVS：逐档播同一段嵌入人声
 * （`audio_play_test_tone()`，「音频链路测试。你好，墨词。」），一次上机
 * 即可判读两件事——
 *   ① 若高档明显响 → 纯设置值问题（把音量调回 75 即可）；
 *   ② 若全档都接近无声、仅 100 档勉强可闻 → 指向功放供电/喇叭链路
 *      （模块规格：NS4150B 5V 才有 2.8W，3V3 仅 ~0.5W；喇叭接口/阻抗）。
 *
 * 用法：pio run -e audio-volume-sweep -t upload
 *      测完烧回：pio run -e inkword-s3-gdeq031t10 -t upload
 */
#include "../audio_player.h"
#include "../es8311.h"

#include <Arduino.h>

/* 扫描档位：末位含默认档 75 与满档 100，另取当前 NVS 实值 25 作起点，
 * 使「现状」与「可接受响度落点」在同一次运行内直接可比 */
static const int SWEEP[] = {25, 50, 75, 100};

void setup()
{
    Serial.begin(115200);
    delay(3000);
    Serial.println("\n### ES8311+NS4150B 音量档响度扫描 ###");
    Serial.println("  每档播同一段嵌入人声，请听记「哪一档开始够响」");
    Serial.println("  判读：高档响=设置值问题；全档弱=功放供电/喇叭链路");

    if (audio_init() != 0) {
        Serial.println("audio_init 失败：查 I2S(4/5/6) 与 I2C(38/39) 接线");
        while (true) delay(1000);
    }
    /* codec 未 probe 到时 audio_init 只告警不致命，这里显式确认一次 */
    Serial.printf("codec I2C init: %s\n\n", es8311_init() == 0 ? "OK" : "FAIL");

    for (unsigned i = 0; i < sizeof(SWEEP) / sizeof(SWEEP[0]); i++) {
        int v = SWEEP[i];
        es8311_set_volume(v);
        Serial.printf("[vol %3d -> REG32 0x%02X] play\n", v, (unsigned)(v * 255 / 100));
        int rc = audio_play_test_tone();
        Serial.printf("  rc=%d\n", rc);
        delay(1200);
    }

    /* 满档复播：排除「首档初始化不全」造成的假象 */
    es8311_set_volume(100);
    Serial.println("\n[repeat @100] 复播确认满档可重复");
    audio_play_test_tone();

    Serial.println("\n=== done ===");
    Serial.println("回填：够响的最低档 → 调 settings_volume 默认/曲线；");
    Serial.println("      全档弱 → 量模块 5V 脚电压与喇叭接口");
}

void loop() { delay(1000); }
