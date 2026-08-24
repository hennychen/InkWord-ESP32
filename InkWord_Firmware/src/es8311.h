/**
 * @file es8311.h
 * @brief ES8311 codec 寄存器驱动 (2026-08-24)
 *
 * ES8311+NS4150B CODEC 模块（取代 MAX98357A+INMP441 双件套）：
 * 播放走 DAC→内部功放驱动 NS4150B；录音走板载/FPC 模拟麦→ADC→DOUT。
 * 寄存器序列移植自 esp-adf 官方 es8311 驱动（ESPRESSIF MIT），
 * 适配本项目：I2C 独占 GPIO38/39、MCLK 省线（SCLK 作 mclk 源，
 * LyraT-Mini 同款方案）、精简 coeff 表（256×fs，8k~48k）。
 *
 * 上层职责划分：audio_player 管 DAC 侧（start/stop/采样率），
 * mic_recorder 管 ADC 侧（录音前 adc_start、毕 adc_stop）；
 * I2S 总线仍由两者各自装卸，本驱动只碰 I2C。
 */
#ifndef INKWORD_ES8311_H
#define INKWORD_ES8311_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief I2C 总线探测 codec 在位（双地址 0x18/0x19 自适应，读 chip id）。
 * @return 实际 7bit 地址；-1 不在位（模块未接/上拉缺失）
 */
int es8311_probe(void);

/**
 * @brief 初始化：装载 I2C + codec 复位/从机模式/mclk 源选择 +
 *        默认 44.1kHz 时钟系数 + DAC/ADC 静音态。
 *        内部已幂等（重复调用直接返回 0）。
 * @return 0 成功；-1 I2C 装载失败；-2 codec 无应答
 */
int es8311_init(void);

/**
 * @brief codec 全掉电（suspend 序列，audio_deinit 深睡收口用）。
 *        I2C 驱动保留（重 init 免重装）；再次播放前须 es8311_init。
 */
void es8311_deinit(void);

/**
 * @brief 重配采样率时钟系数（I2S 重装后调用；8k~48k）。
 * @return 0 成功；-1 采样率不受支持
 */
int es8311_set_sample_rate(uint32_t rate);

/** DAC 起播：解除静音 + 上电 + SDP 输出使能（含防爆破音时序）。 */
void es8311_dac_start(void);

/** DAC 停播：静音 + 输出使能关（轻量，不掉全局电源）。 */
void es8311_dac_stop(void);

/**
 * ADC 起录：上电 + AMIC 选通 + PGA 增益 + SDPOUT 使能。
 * @param gain_lvl PGA 档 0~7（0/6/12/18/24/30/36/42 dB）
 * @return 0 成功；非 0 未初始化/参数越界
 */
int es8311_adc_start(int gain_lvl);

/** ADC 停录：静音 + SDPOUT 关（轻量）。 */
void es8311_adc_stop(void);

/**
 * @brief DAC 数字音量（0~100 映射到 REG32 0~255）。
 */
int es8311_set_volume(int vol_0_100);

/** init 后在位标志（日志排查用）。 */
bool es8311_present(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_ES8311_H */
