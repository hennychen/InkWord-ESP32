/**
 * @file epd_bus.cpp
 * @brief L0 公共 SPI 原语层实现（T1.1+T1.8）
 *
 * 各函数的时序口径与陷阱注释自六份面板单元原样收敛（序列字节不动
 * 铁律：本文件只承载传输/等待/判族骨架，面板知识仍在 panel_*.cpp）。
 * 迁移源逐函数注明；诊断输出统一经 DIAG_LOG（生产态空展开）。
 */
#include "epd_bus.h"
#include "../gpio_config.h"

#include <Arduino.h>
#include <SPI.h>

extern "C" {

/* —— SPI 事务原语（迁移源：六面板 epd_cmd/epd_dat/epd_write_buf
 * 同构实现，wft0290 注释口径：transfer 逐字节连发，SPI.writeBytes
 * 在 ESP32-S3 Arduino core 有 RAM 不落地陷阱，禁用） ——
 *
 * 2026-09-07 SPI 频率提升 4MHz → 8MHz：SSD1677 datasheet 确认
 * 最高 20MHz（fSCL max 20MHz），SSD1619/1680 均支持 10MHz+，
 * UC8151 支持 10MHz。8MHz 全面板安全，96KB 传输 192ms → 96ms。 */
static const uint32_t k_spi_hz = 8000000;

void bus_cmd(uint8_t c)
{
    SPI.beginTransaction(SPISettings(k_spi_hz, MSBFIRST, SPI_MODE0));
    digitalWrite(EPD_CS_PIN, LOW);
    digitalWrite(EPD_DC_PIN, LOW);    /* DC=0 命令 */
    SPI.transfer(c);
    digitalWrite(EPD_CS_PIN, HIGH);
    SPI.endTransaction();
}

void bus_dat(uint8_t d)
{
    SPI.beginTransaction(SPISettings(k_spi_hz, MSBFIRST, SPI_MODE0));
    digitalWrite(EPD_CS_PIN, LOW);
    digitalWrite(EPD_DC_PIN, HIGH);   /* DC=1 数据 */
    SPI.transfer(d);
    digitalWrite(EPD_CS_PIN, HIGH);
    SPI.endTransaction();
}

void bus_dat_stream(const uint8_t *buf, size_t n)
{
    SPI.beginTransaction(SPISettings(k_spi_hz, MSBFIRST, SPI_MODE0));
    digitalWrite(EPD_CS_PIN, LOW);
    digitalWrite(EPD_DC_PIN, HIGH);
    for (size_t i = 0; i < n; i++) SPI.transfer(buf[i]);
    digitalWrite(EPD_CS_PIN, HIGH);
    SPI.endTransaction();
}

/* 自定义流三件套（OPM021EB write_ram_frame 骨架抽出）：open 后
 * 逐字节 put、close 收口，全程单 CS 事务（陷阱②：CS↑ 重置 COG
 * 地址计数器，帧行重排流不得分段） */
void bus_dat_begin(void)
{
    SPI.beginTransaction(SPISettings(k_spi_hz, MSBFIRST, SPI_MODE0));
    digitalWrite(EPD_CS_PIN, LOW);
    digitalWrite(EPD_DC_PIN, HIGH);
}

void bus_dat_put(uint8_t b)
{
    SPI.transfer(b);
}

void bus_dat_end(void)
{
    digitalWrite(EPD_CS_PIN, HIGH);
    SPI.endTransaction();
}

/* —— BUSY 等待（迁移源：wait_idle_level / panel_wait_idle 六面板同构） —— */

void bus_wait_idle(const epd_panel_desc_t *d, uint32_t timeout_ms)
{
    const uint32_t t0 = millis();
    while (digitalRead(EPD_BUSY_PIN) == d->busy_level &&
           millis() - t0 < timeout_ms)
        delay(10);
}

/* 两段式（迁移源：wait_refresh_done 六面板同构，wft0290 口径：
 * 置忙延迟打印前捕获）。忙电平文案统一按 desc.busy_level 数值化 */
bool bus_wait_busy(const epd_panel_desc_t *d, uint32_t timeout_ms)
{
    const uint32_t t0 = millis();
    while (digitalRead(EPD_BUSY_PIN) != d->busy_level &&
           millis() - t0 < 300)
        delay(2);
    const bool asserted =
        digitalRead(EPD_BUSY_PIN) == d->busy_level;
#if INKWORD_EPD_DIAG
    const uint32_t t_enter = millis() - t0;
#endif
    const uint32_t t1 = millis();
    while (digitalRead(EPD_BUSY_PIN) == d->busy_level &&
           millis() - t1 < timeout_ms)
        delay(10);
    DIAG_LOG("%s refresh busy: %s @%ums, active %ums",
             d->name, asserted ? "asserted" : "never",
             (unsigned)t_enter, (unsigned)(millis() - t1));
    return asserted;
}

/* —— bring-up 通电自检（迁移源：wft0290/opm021eb detect_alive 同款，
 * 判族电平泛化为 !busy_level；RST 前置 HIGH 取 wft0290 三轮实测 200ms
 * 保守口径，opm021eb 原 10ms 同族安全） —— */
int bus_detect_alive(const epd_panel_desc_t *d)
{
    pinMode(EPD_RESET_PIN, OUTPUT);
    pinMode(EPD_CS_PIN, OUTPUT);
    pinMode(EPD_DC_PIN, OUTPUT);
    pinMode(EPD_BUSY_PIN, INPUT);
    digitalWrite(EPD_CS_PIN, HIGH);
    digitalWrite(EPD_DC_PIN, LOW);

    /* RST：HIGH 200ms → LOW rst_pulse_ms → HIGH */
    digitalWrite(EPD_RESET_PIN, HIGH);
    delay(200);
    digitalWrite(EPD_RESET_PIN, LOW);
    delay(d->rst_pulse_ms);
    digitalWrite(EPD_RESET_PIN, HIGH);

    /* 证据①：RST 释放后 5s 窗口观察 BUSY 忙→闲往返（COG boot 自检） */
    const uint32_t t0 = millis();
    bool saw_high = false, saw_low = false;
    while (millis() - t0 < 5000 && !(saw_high && saw_low)) {
        if (digitalRead(EPD_BUSY_PIN)) saw_high = true;
        else saw_low = true;
        delay(10);
    }
    const bool cog_alive = saw_high && saw_low;

    /* 证据②：静置 300ms 后采样 5 次取众数（期望空闲电平 !busy_level：
     * UC 族 HIGH / SSD16xx LOW） */
    delay(300);
    int high_cnt = 0;
    for (int i = 0; i < 5; i++) {
        if (digitalRead(EPD_BUSY_PIN)) high_cnt++;
        delay(10);
    }
    const bool idle_high = high_cnt >= 3;
    const bool idle_expected = d->busy_level == 0;

    DIAG_LOG("RST pulse: H%s L%s (alive=%d) | BUSY idle: %s (%d/5 HIGH)",
             saw_high ? "+" : "-", saw_low ? "+" : "-",
             (int)cog_alive, idle_high ? "HIGH" : "LOW", high_cnt);

    if (!cog_alive) {
        DIAG_LOG("COG no answer: check FPC seat / "
                 "VCI 3.3V / BS=LOW(4-line SPI) wiring");
        return -1;
    }
    if (idle_high != idle_expected) {
        DIAG_LOG("idle %s — NOT this panel's family "
                 "(desc expects %s); check FPC seat / wiring",
                 idle_high ? "HIGH (SSD16xx traits)" : "LOW (UC traits)",
                 idle_expected ? "HIGH" : "LOW");
        return -1;
    }
    return 0;
}

/* —— 状态读诊断（原样迁自 epd_driver.cpp epd_diag_read_status，
 * E042A13 bring-up 2026-08-22 位掩回读口径） —— */
uint8_t bus_diag_read_status(uint8_t cmd, bool delay_50ms)
{
    pinMode(EPD_SCK_PIN, OUTPUT);
    pinMode(EPD_MOSI_PIN, OUTPUT);
    pinMode(EPD_DC_PIN, OUTPUT);
    pinMode(EPD_CS_PIN, OUTPUT);
    digitalWrite(EPD_CS_PIN, LOW);
    digitalWrite(EPD_DC_PIN, LOW);   /* 命令阶段 */
    digitalWrite(EPD_SCK_PIN, LOW);
    delayMicroseconds(2);
    for (int i = 7; i >= 0; i--) {   /* mode0：SCK 低电平期放数据 */
        digitalWrite(EPD_MOSI_PIN, (cmd >> i) & 0x01);
        delayMicroseconds(2);
        digitalWrite(EPD_SCK_PIN, HIGH);
        delayMicroseconds(2);
        digitalWrite(EPD_SCK_PIN, LOW);
    }
    digitalWrite(EPD_DC_PIN, HIGH);  /* 数据阶段 */
    pinMode(EPD_MOSI_PIN, INPUT);    /* SDA 交还 COG 驱动 */
    if (delay_50ms) delay(50);       /* SSD1619 版本读需 50ms（demo 同款） */
    delayMicroseconds(2);
    uint8_t flg = 0;
    for (int i = 7; i >= 0; i--) {   /* COG 驱动位，MCU 上升沿采样 */
        digitalWrite(EPD_SCK_PIN, HIGH);
        delayMicroseconds(2);
        flg = (uint8_t)((flg << 1) | (digitalRead(EPD_MOSI_PIN) ? 1 : 0));
        digitalWrite(EPD_SCK_PIN, LOW);
        delayMicroseconds(2);
    }
    digitalWrite(EPD_CS_PIN, HIGH);
    digitalWrite(EPD_DC_PIN, LOW);
    return flg;
}

/* ---- 族标准电源序列（P2d 家族化：byte 级一致者单点，见头注释） ---- */

void bus_ssd16_power_off(const epd_panel_desc_t *d, bool *ready)
{
    /* SSD16xx 标准关电（GxEPD2 GDEY042Z98/_PowerOff 同款；wf0270 /
     * e042a13 / e042a13bw 三家一致）：0x22/0xC3 + 0x20。完成后归零
     * *ready —— 下次刷新完整重配（无状态铁律，不赌关电后 RAM
     * 窗口/计数器存活） */
    if (!*ready) return;
    bus_cmd(0x22); bus_dat(0xC3);
    bus_cmd(0x20);
    bus_wait_idle(d, d->busy_timeout_ms);
    *ready = false;
}

void bus_ssd16_deep_sleep(bool *ready)
{
    /* Waveshare Sleep(_new) 一比一：0x10 check 0x01 深睡（~µA 级），
     * RST 硬复位唤醒 + panel_init 重初始化（三家 demo 实证同款） */
    bus_cmd(0x10); bus_dat(0x01);
    *ready = false;
}

void bus_uc_power_off(const epd_panel_desc_t *d, bool *ready)
{
    /* GxEPD2 _PowerOff 忠实（opm021eb / wft0290 一致）：0x02 关高压
     * rails（VCI 3.3V 保持供电）。完成后归零 *ready —— 下次刷新
     * 完整重配 */
    if (!*ready) return;
    bus_cmd(0x02);
    bus_wait_idle(d, 1000);
    *ready = false;
}

void bus_uc_deep_sleep(bool *ready)
{
    /* UC 系深睡 0x07/0xA5（~µA 级）+ 200ms 稳定窗，RST 硬复位唤醒
     * + uc_init 重初始化（两家面板一致） */
    bus_cmd(0x07);
    bus_dat(0xA5);
    delay(200);
    *ready = false;
}

/* desc.ops.diag 族标准实现（原 epd_driver_init 2d 段逻辑下沉，T1.8：
 * L3 不再感知控制器型号）。连读两次看稳定性；函数体整体门控，
 * 生产态空实现（链接符号保留，零体积零延迟） */
void bus_diag_uc(void)
{
#if INKWORD_EPD_DIAG
    const uint8_t st1 = bus_diag_read_status(0x71, false);
    const uint8_t st2 = bus_diag_read_status(0x71, false);
    const uint8_t expect = 0x02;   /* UC8176 系 FLG POF 默认 */
    DIAG_LOG("status read(0x71): 0x%02X/0x%02X %s",
             st1, st2,
             st1 == expect || st2 == expect
                 ? "(EXPECTED: SPI LOOP OK, COG responded)" :
             st1 == 0xFF && st2 == 0xFF
                 ? "(floating: cmd lost / no COG drive / read timing)" :
             st1 == 0x00 && st2 == 0x00
                 ? "(stuck LOW: short / no drive)" :
                 "(unexpected value: check controller)");
#endif
}

void bus_diag_ssd16(void)
{
#if INKWORD_EPD_DIAG
    const uint8_t st1 = bus_diag_read_status(0x2F, true);
    const uint8_t st2 = bus_diag_read_status(0x2F, true);
    const uint8_t expect = 0x01;   /* SSD1619 实证值 */
    DIAG_LOG("status read(0x2F): 0x%02X/0x%02X %s",
             st1, st2,
             st1 == expect || st2 == expect
                 ? "(EXPECTED: SPI LOOP OK, COG responded)" :
             st1 == 0xFF && st2 == 0xFF
                 ? "(floating: cmd lost / no COG drive / read timing)" :
             st1 == 0x00 && st2 == 0x00
                 ? "(stuck LOW: short / no drive)" :
                 "(unexpected value: check controller)");
#endif
}

} /* extern "C" */
