/**
 * @file max17048.c
 * @brief MAX17048 电量计驱动实现（v1.2 T2.6，寄存器口径见 max17048.h）
 *
 * 寄存器（datasheet Rev 1 表 2，16bit 大端，经 i2c_bus_read_reg16）：
 *   VCELL 0x02：78.125µV/LSB；SOC 0x04：1/256%/LSB；
 *   MODE 0x06：睡眠控制（不用）；VERSION 0x08：固件版本（0x001_）；
 *   CRATE 0x16：充放电速率 0.208%/hr（v1.5 低电预警可选）。
 */
#include "max17048.h"
#include "i2c_bus.h"

#include "debug_log.h"

#include <stdbool.h>

static const char *TAG = "M17048";

#define M17048_REG_VCELL   0x02
#define M17048_REG_SOC     0x04
#define M17048_REG_VERSION 0x08

static bool s_ready = false;

int max17048_init(void)
{
    if (i2c_bus_init() != 0) return -1;

    uint16_t ver = 0;
    if (i2c_bus_read_reg16(MAX17048_I2C_ADDR, M17048_REG_VERSION, &ver) != 0) {
        LOG_W("max17048 not responding at 0x%02x (module absent?)",
              MAX17048_I2C_ADDR);
        return -2;
    }
    if ((ver >> 8) != 0x00 || (ver & 0xF0) != 0x10) {
        /* 官方 datasheet 3.4.7：VERSION 恒 0x001_（低 4 位保留位因硅批次
         * 有浮动，按高 12 位校验；全错大概率挂了别的设备） */
        LOG_W("max17048 version mismatch: 0x%04x", ver);
        return -2;
    }
    s_ready = true;
    LOG_I("max17048 ready (version 0x%04x)", ver);
    return 0;
}

int max17048_ready(void)
{
    return s_ready ? 0 : -1;
}

int max17048_percent(void)
{
    if (!s_ready) return -1;
    uint16_t soc = 0;
    if (i2c_bus_read_reg16(MAX17048_I2C_ADDR, M17048_REG_SOC, &soc) != 0)
        return -1;
    int pct = (soc * 100 + 128) / 256;   /* 四舍五入：/256 换算百分比 */
    if (pct > 100) pct = 100;            /* 充电满溢出钳位 */
    return pct;
}

int max17048_voltage_mv(void)
{
    if (!s_ready) return -1;
    uint16_t vc = 0;
    if (i2c_bus_read_reg16(MAX17048_I2C_ADDR, M17048_REG_VCELL, &vc) != 0)
        return -1;
    /* 78.125µV/LSB = 1LSB×1000/128 µmV 换算整毫伏（×1000/128 = ×125/16），
     * 四舍五入用 +8；1S 配置 cell 计数 1 直读 */
    return (int)((vc * 125 + 8) / 16);
}
