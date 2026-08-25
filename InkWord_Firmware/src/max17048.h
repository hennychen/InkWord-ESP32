/**
 * @file max17048.h
 * @brief MAX17048 电量计驱动（v1.2 T2.6，I2C 复用方案）
 *
 * 挂 38/39 现有总线（0x36，与 ES8311 0x18 无冲突；无空闲 ADC GPIO，
 * 分压直采方案已否决——见任务分解被否决项）。ModelGauge 算法芯片
 * 内置（上电自学习），主机只读 SOC/VCELL 即可，无配置序列。
 *
 * 硬件依赖：模块到位前 init 返回 -1，调用方占位显示（INFO 页 "--"、
 * 心跳报文回退 100）——代码就绪待硬件，失败回退保持占位顺延。
 */
#ifndef INKWORD_MAX17048_H
#define INKWORD_MAX17048_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX17048_I2C_ADDR 0x36   /**< 7bit 固定地址 */

/**
 * @brief 初始化：i2c_bus 装载 + VERSION 寄存器探测（0x001_）。
 * @return 0 在位；-1 总线失败；-2 探测无应答（模块不在位）。
 */
int max17048_init(void);

/** 模块是否在位（init 成功过）。 */
int max17048_ready(void);

/**
 * @brief 电量百分比（SOC 寄存器 1/256% 单位换算）。
 * @return 0~100；-1 读失败/不在位。
 */
int max17048_percent(void);

/**
 * @brief 电池电压毫伏（VCELL 寄存器 78.125µV LSB，1S 配置直读）。
 * @return mV（~3600-4200）；-1 读失败/不在位。
 */
int max17048_voltage_mv(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_MAX17048_H */
