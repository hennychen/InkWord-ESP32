/**
 * @file i2c_bus.h
 * @brief 公共 I2C 总线抽象（v1.2 T2.6：ES8311 + MAX17048 共享 38/39）
 *
 * 单主设备单总线（I2C0 @ GPIO38/39，参数宏现由 ES8311 引脚宏承载——
 * 同物理总线，统一改名留待后续清理）。本模块收口：
 *   - 幂等装载（param_config + driver_install，首个调用者触发）；
 *   - FreeRTOS 互斥锁（多任务安全：音频任务/心跳任务并发读电量与
 *     codec 寄存器写竞争在此串行化）；
 *   - 寄存器级原语（8bit 寄存器 + 16bit 大端寄存器，MAX17048 数据
 *     寄存器为大端双字节）。
 * 设备探测语义归各驱动（es8311_probe / max17048_init），总线装载
 * 失败才悲观返回；单个设备不在位不影响总线与其他设备。
 */
#ifndef INKWORD_I2C_BUS_H
#define INKWORD_I2C_BUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 幂等装载总线（配置 + 驱动安装 + 互斥锁创建）。
 * @return 0 成功/已装载；-1 配置或安装失败（总线不可用）。
 */
int i2c_bus_init(void);

/** 写 8bit 寄存器（addr 为 7bit 地址）。0 成功 / -1 失败。 */
int i2c_bus_write_reg(uint8_t addr, uint8_t reg, uint8_t val);

/** 读 8bit 寄存器。0 成功 / -1 失败。 */
int i2c_bus_read_reg(uint8_t addr, uint8_t reg, uint8_t *out);

/**
 * @brief 读 16bit 大端寄存器（MAX17048 VCELL/SOC 等）。
 *        写侧暂无需求（本项目 fuel gauge 只读），按需再加。
 */
int i2c_bus_read_reg16(uint8_t addr, uint8_t reg, uint16_t *out);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_I2C_BUS_H */
