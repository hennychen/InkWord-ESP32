/**
 * @file epd_bus.h
 * @brief L0 公共 SPI 原语层（T1.1，修 B1）—— 面板单元底层收敛
 *
 * 六份手写面板的重复底层（epd_cmd×6 / epd_dat×6 / wait_refresh_done×6 /
 * detect_alive 模板）收敛为单一实现；SPI 陷阱知识单点沉淀：
 *   ① SPI.writeBytes 在 ESP32-S3 Arduino core 有 RAM 不落地陷阱
 *      （E042A13 bring-up boot6/7/8 三轮实锤），批量写必须 transfer
 *      逐字节连发 —— 见 bus_dat_stream；
 *   ② CS↑ 重置 COG 地址计数器（OPM021EB 六轮 250 段事务白屏证伪，
 *      八轮单 CS 事务连续流定稿）—— 帧行重排等自定义流必须经
 *      bus_dat_begin/put/end 保持单事务；
 *   ③ 0x12 刷新后 BUSY 两段式等待（置忙容忍窗 + 释放窗）——
 *      bus_wait_busy 兼作「命令是否达 COG」现场判据。
 *
 * 面板文件保留：命令序列表、PSR/LUT 常量、行宽特例（K_ROW_BYTES）、
 * 真值表注释（铁律：序列字节不动）。
 *
 * 诊断门控（T1.8，修 E2）：bring-up 诊断输出经 DIAG_LOG 宏，生产构建
 * （INKWORD_EPD_DIAG=0）空展开，零体积零延迟；demo/probe env 置 1 完整
 * 保留。desc.ops.diag 状态读回调按控制器族提供标准实现，L3 不再感知
 * 面板型号（is_ssd16 分支下沉）。
 */
#ifndef INKWORD_EPD_BUS_H
#define INKWORD_EPD_BUS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "../epd_panel.h"

/* bring-up 诊断编译开关（T1.8）：[env] 默认 0（生产），
 * inkword-s3-demo / 探针 env 置 1。未定义时按 0 处理 */
#ifndef INKWORD_EPD_DIAG
#define INKWORD_EPD_DIAG 0
#endif

#if INKWORD_EPD_DIAG
#define DIAG_LOG(fmt, ...) \
    Serial.printf("[EPD-DIAG] " fmt "\n", ##__VA_ARGS__)
#else
#define DIAG_LOG(fmt, ...) do { } while (0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* —— SPI 事务原语（epd_driver_init 已 SPI.begin，此处事务直发） —— */

void bus_cmd(uint8_t c);
/* 单字节命令事务（DC=0） */

void bus_dat(uint8_t d);
/* 单字节数据事务（DC=1） */

void bus_dat_stream(const uint8_t *buf, size_t n);
/* 批量数据：单 CS 事务 transfer 逐字节连发（陷阱①，勿改 writeBytes） */

void bus_dat_begin(void);
/* 自定义数据流开事务（CS↓ + DC=1，行宽重排等特例用；陷阱②：
 * 必须与 bus_dat_end 成对，中间不得有其它事务，保持单 CS 连续流） */

void bus_dat_put(uint8_t b);
/* 自定义数据流发一字节（仅在 begin/end 之间调用） */

void bus_dat_end(void);
/* 自定义数据流关事务（CS↑） */

/* —— BUSY 等待（极性/超时取 desc，面板轴泛化） —— */

void bus_wait_idle(const epd_panel_desc_t *d, uint32_t timeout_ms);
/* 单段：等 BUSY 回空闲电平（上电/关电路径用） */

bool bus_wait_busy(const epd_panel_desc_t *d, uint32_t timeout_ms);
/* 两段式（0x12/0x20 刷新后）：① 等 BUSY 进入忙电平（≤300ms 容忍命令
 * 置位延迟）；② 等释放（≤timeout_ms）。返回 true=忙电平正常置位；
 * false=从未置位（刷新命令未达 COG，SPI 硬件排查判据）。诊断输出经
 * DIAG_LOG（busy 电平/置位延迟/持续时长） */

/* —— bring-up 通电自检 —— */

int bus_detect_alive(const epd_panel_desc_t *d);
/* RST 脉冲 + BUSY 忙→闲往返 + 空闲电平众数判族（WFT0290/OPM021EB
 * 同款）。判族口径：期望空闲电平 = !busy_level（UC 族 idle HIGH /
 * SSD16xx idle LOW）。异常 → fail-safe -1（epd_driver_init LOG_E 退出） */

/* —— 状态读诊断（T1.8：desc.ops.diag 标准实现） —— */

uint8_t bus_diag_read_status(uint8_t cmd, bool delay_50ms);
/* SPI 位掩回读（须在 SPI.begin 之前执行，GPIO 矩阵未被占用；
 * E042A13 bring-up 2026-08-22 原样迁自 epd_driver.cpp） */

void bus_diag_uc(void);
/* UC 族（UC8151/UC8253）FLG 0x71 双读（expect 0x02）—— desc.ops.diag */

void bus_diag_ssd16(void);
/* SSD16xx 族版本读 0x2F 双读（expect 0x01，SSD1619 实证）——
 * desc.ops.diag；IL91874 无 FLG/版本寄存器，面板填 NULL 跳过 */

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_EPD_BUS_H */
