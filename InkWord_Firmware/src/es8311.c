/**
 * @file es8311.c
 * @brief ES8311 codec 寄存器驱动实现 (2026-08-24；bring-up 定型 2026-08-27)
 *
 * 序列对齐 esp_codec_dev 1.5.6（xiaozhi-esp32 57 块量产板同款）。
 * bring-up 根因：初版自 esp-adf v2.5 移植，init 尾部 REG00=0x00
 * （挂起/非正常态），芯片全程未进入工作态——嘶嘶/无声/丝印互换
 * 假象皆源于此；2026-08-27 修正为 REG00=0x80 从机正常态 +
 * MCLK 实线拓扑（GPIO0 输出 256×fs，显式 gpio_matrix_out 路由）。
 *
 * 职责边界：本驱动只碰 I2C 寄存器；I2S 总线由 audio_player（DAC 侧）
 * 与 mic_recorder（ADC 侧）各自装卸，调用方在 I2S 重装后调
 * es8311_set_sample_rate 同步时钟系数。
 */
#include "es8311.h"
#include "gpio_config.h"
#include "i2c_bus.h"    /* v1.2 T2.6：I2C 收口（与 MAX17048 共享 38/39） */
#include "debug_log.h"

#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"   /* probe 退避重试 vTaskDelay（2026-08-28） */
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "ES8311";

/* ---- 寄存器地址（与 es8311.h 寄存器定义一致） ---- */
#define REG_RESET       0x00
#define REG_CLK_MGR01   0x01   /* mclk 源选择、codec 时钟使能 */
#define REG_CLK_MGR02   0x02   /* pre_div / pre_multi */
#define REG_CLK_MGR03   0x03   /* adc fs_mode / osr */
#define REG_CLK_MGR04   0x04   /* dac osr */
#define REG_CLK_MGR05   0x05   /* adc_div / dac_div */
#define REG_CLK_MGR06   0x06   /* bclk div / invert */
#define REG_CLK_MGR07   0x07   /* lrck 高 8 位 */
#define REG_CLK_MGR08   0x08   /* lrck 低 8 位 */
#define REG_SDPIN       0x09   /* DAC SDP */
#define REG_SDPOUT      0x0A   /* ADC SDP */
#define REG_SYS0B       0x0B
#define REG_SYS0C       0x0C
#define REG_SYS0D       0x0D   /* 全局电源 */
#define REG_SYS0E       0x0E   /* 电源 */
#define REG_SYS10       0x10
#define REG_SYS11       0x11
#define REG_SYS12       0x12   /* DAC 电源 */
#define REG_SYS13       0x13   /* HP 驱动输出使能（DAC→喇叭通路） */
#define REG_SYS14       0x14   /* PGA / DMIC */
#define REG_ADC15       0x15   /* ADC ramp / DMIC */
#define REG_ADC16       0x16   /* MIC PGA 增益 */
#define REG_ADC17       0x17   /* ADC 音量 */
#define REG_ADC1B       0x1B   /* ADC 校准（官方 init 写 0x0A） */
#define REG_ADC1C       0x1C   /* ADC 校准（官方 init 写 0x6A） */
#define REG_DAC31       0x31   /* DAC mute */
#define REG_DAC32       0x32   /* DAC 音量 */
#define REG_DAC37       0x37   /* DAC ramp */
#define REG_GP44        0x44   /* 内部参考信号选择 */
#define REG_GP45        0x45   /* 全局控制 */

/* ---- 时钟系数表（256×fs；本项目只用 8k/16k/44.1k/48k，保留全表） ---- */
struct _coeff_div {
    uint32_t mclk;
    uint32_t rate;
    uint8_t pre_div;
    uint8_t pre_multi;   /* 1/2/4/8 */
    uint8_t adc_div;
    uint8_t dac_div;
    uint8_t fs_mode;     /* 0=SS / 1=DS */
    uint8_t lrck_h;
    uint8_t lrck_l;
    uint8_t bclk_div;
    uint8_t adc_osr;
    uint8_t dac_osr;
};

/* ---- DAC 数字音量状态（2026-08-27 音量设置） ----
 * 驱动内唯一真相源：set_volume 保存，dac_start 起播时回写（替代
 * 曾硬编码的 0xBF=0dB，否则每次起播重置音量）；NVS 持久化镜像由
 * settings_ui 管理，启动后同步一次（main.cpp audio_init 后）。
 * 默认 75：75*255/100=191=0xBF，与历史 0dB 听感严格一致 */
#define ES8311_VOL_DEFAULT 75
static uint8_t s_volume = ES8311_VOL_DEFAULT;

static const struct _coeff_div coeff_div[] = {
    /* mclk       rate    pre  mul  adc  dac  fs  lrck_h lrck_l bclk  osr_a osr_d */
    {12288000,  8000,  0x06, 1, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {16384000,  8000,  0x08, 1, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {8192000,   8000,  0x04, 1, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {6144000,   8000,  0x03, 1, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {4096000,   8000,  0x02, 1, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {3072000,   8000,  0x01, 1, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {2048000,   8000,  0x01, 1, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {11289600, 11025,  0x04, 1, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {5644800,  11025,  0x02, 1, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {2822400,  11025,  0x01, 1, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {12288000, 12000,  0x04, 1, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {6144000,  12000,  0x02, 1, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {3072000,  12000,  0x01, 1, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {12288000, 16000,  0x03, 1, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {16384000, 16000,  0x04, 1, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {8192000,  16000,  0x02, 1, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {6144000,  16000,  0x03, 2, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {4096000,  16000,  0x01, 1, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {3072000,  16000,  0x03, 4, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {2048000,  16000,  0x01, 2, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {11289600, 22050,  0x02, 1, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {5644800,  22050,  0x01, 1, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {12288000, 24000,  0x02, 1, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {6144000,  24000,  0x01, 1, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {3072000,  24000,  0x01, 2, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {12288000, 32000,  0x03, 2, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {16384000, 32000,  0x02, 1, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {8192000,  32000,  0x01, 1, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {4096000,  32000,  0x01, 2, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {11289600, 44100,  0x01, 1, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {5644800,  44100,  0x01, 2, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {12288000, 48000,  0x01, 1, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
    {6144000,  48000,  0x01, 2, 1, 1, 0, 0x00, 0xff, 0x04, 0x10, 0x10},
};

static int s_addr = -1;          /* 探测到的 7bit 地址（-1=未探测/不在位） */
static bool s_inited = false;

/* ---- I2C 底层（v1.2 T2.6 收口 i2c_bus：互斥锁/装载归公共层，
 * 本层只保留 7bit 地址状态与读-改-写语义） ---- */
static int i2c_write(uint8_t reg, uint8_t val)
{
    if (s_addr < 0) return -1;
    return i2c_bus_write_reg((uint8_t)s_addr, reg, val);
}

static int i2c_read(uint8_t reg, uint8_t *out)
{
    if (s_addr < 0) return -1;
    return i2c_bus_read_reg((uint8_t)s_addr, reg, out);
}

/* 读-改-写（只改 mask 覆盖位，其余保留） */
static int i2c_rmw(uint8_t reg, uint8_t mask, uint8_t val)
{
    uint8_t cur;
    if (i2c_read(reg, &cur) != 0) return -1;
    return i2c_write(reg, (uint8_t)((cur & ~mask) | (val & mask)));
}

/* ---- 探测：双地址自适应 ---- */
int es8311_probe(void)
{
    /* 先尝试默认 0x18，失败再试 0x19 */
    static const int addrs[] = { ES8311_I2C_ADDR, ES8311_I2C_ADDR ^ 1 };
    for (int i = 0; i < 2; i++) {
        s_addr = addrs[i];
        uint8_t id = 0;
        if (i2c_read(0xFD, &id) == 0) {   /* chip id1 */
            LOG_I("codec found at 0x%02x (id=0x%02x)", s_addr, id);
            return s_addr;
        }
    }
    s_addr = -1;
    return -1;
}

/* ---- 时钟系数查找 ---- */
static int find_coeff(uint32_t rate)
{
    /* 本项目 mclk = 256×rate（SCLK 作 mclk 源时等效） */
    uint32_t mclk = rate * 256;
    for (size_t i = 0; i < sizeof(coeff_div) / sizeof(coeff_div[0]); i++) {
        if (coeff_div[i].rate == rate && coeff_div[i].mclk == mclk)
            return (int)i;
    }
    return -1;
}

/* ---- 初始化 ---- */
int es8311_init(void)
{
    if (s_inited) return 0;

    /* 1. 装载 I2C 总线（v1.2 T2.6：收口 i2c_bus，与 MAX17048 共享
     * 互斥锁；参数宏仍由本模块引脚宏承载，驱动仅首个调用者装载） */
    if (i2c_bus_init() != 0)
        return -1;

    /* 2. 探测 codec（失败保留总线装载——共享总线下设备在位性独立，
     * 心跳电量读数仍可用）。
     * 三级递进（2026-08-28 实测）：退避重试（codec 响应滞后场景）
     * → 总线恢复（从机事务中途失宿主拉死 SDA 的死锁场景，
     * 9×SCL+STOP）→ 再退避。单次 probe + 纯延时已被实测否决。 */
    int rc = es8311_probe();
    if (rc < 0) {
        vTaskDelay(pdMS_TO_TICKS(300));
        rc = es8311_probe();
    }
    if (rc < 0) {
        i2c_bus_recover();
        rc = es8311_probe();
    }
    if (rc < 0) {
        vTaskDelay(pdMS_TO_TICKS(300));
        rc = es8311_probe();
    }
    if (rc < 0) {
        LOG_E("codec not found on I2C (check module wiring)");
        return -2;
    }

    /* 3. 初始化序列逐项转录自 esp_codec_dev 1.5.6 es8311_open
     * （xiaozhi-esp32 57 块量产板同款，2026-08-27 对照修复：
     * 原移植自 esp-adf v2.5，与 esp_codec_dev 有 6 处寄存器差异） */
    i2c_write(REG_SYS0D, 0xFA);           /* 模拟断电起步 */
    i2c_write(REG_GP44, 0x08);            /* I2C 抗噪增强（官方连写两次） */
    i2c_write(REG_GP44, 0x08);
    i2c_write(REG_CLK_MGR01, 0x30);
    i2c_write(REG_CLK_MGR02, 0x00);
    i2c_write(REG_CLK_MGR03, 0x10);
    i2c_write(REG_ADC16, 0x24);           /* 新增：官方 init 固定值 */
    i2c_write(REG_CLK_MGR04, 0x10);
    i2c_write(REG_CLK_MGR05, 0x00);
    i2c_write(REG_SYS0B, 0x00);
    i2c_write(REG_SYS0C, 0x00);
    i2c_write(REG_SYS10, 0x1F);
    i2c_write(REG_SYS11, 0x7F);

    /* REG00 = 0x80：从机模式正常态（官方 open/start 恒保持 0x80；
     * 0x00 非正常态——本次移植自始至终以 0x00 收尾，芯片长期处于
     * 挂起/半复位态，疑为全程嘶嘶/无声的统一根因，2026-08-27
     * 对照 esp_codec_dev 1.5.6 修正） */
    i2c_write(REG_RESET, 0x80);

    /* REG01: bit7=mclk 源（0=MCLK 脚 / 1=SCLK 内部），bit5:4=时钟使能；
     * bit6=mclk 反相（0=不反相） */
    uint8_t reg01 = 0x3F;                 /* 使能全部时钟 */
    if (ES8311_MCLK_PIN < 0) {
        reg01 |= 0x80;                    /* 选 SCLK 作 mclk 源（省线） */
    }
    i2c_write(REG_CLK_MGR01, reg01);

    /* REG06 bit5: SCLK 反相位显式清零（官方 invert_sclk=false 路径） */
    i2c_rmw(REG_CLK_MGR06, 0x20, 0x00);

    i2c_write(REG_SYS13, 0x10);           /* HP 驱动输出使能 */
    i2c_write(REG_ADC1B, 0x0A);           /* 新增：官方 init 固定值 */
    i2c_write(REG_ADC1C, 0x6A);           /* 新增：官方 init 固定值 */
    i2c_write(REG_GP44, 0x58);            /* 0x58 非 0x50：内部参考 ADCL+DACR
                                            + bit3 I2C 抗噪（bit3 恒置位） */

    /* 4. 默认 44.1kHz 时钟系数 —— 先置位 s_inited：set_sample_rate
     * 开头有 !s_inited 门卫，置位前调用会被拦（2026-08-26 实测
     * 启动日志 W 级告整定位：系数未写入 codec，DAC 跑复位默认
     * 分频，播放变调/无声）。时钟寄存器 REG02-08 与后续静默态
     * REG31/32/17/0E/12/14/0D/15/37/45 无交集，顺序安全 */
    s_inited = true;
    /* 默认系数与 I2S 装载同源（2026-08-28）：曾硬编码 44100，I2S 侧统一
     * 48k 后即成双源失配隐患（I2S 48k / codec 44.1k 系数， dac_start
     * 先于 set_rate 的窗口变调） */
    if (es8311_set_sample_rate(I2S_SAMPLE_RATE) != 0) {
        s_inited = false;
        LOG_W("default %dHz coeff not found (non-fatal)", I2S_SAMPLE_RATE);
    }
    
    /* 5. 默认静音态（DAC/ADC 均不上电） */
    i2c_write(REG_SDPIN, 0x0C);           /* DAC SDP：16bit I2S 格式（官方 0x0C，
                                            位定义 bit[3:2]=字长 bit[1:0]=I2S） */
    i2c_write(REG_SDPOUT, 0x4C);          /* ADC SDP：16bit I2S + bit6=1 关（静默态） */
    i2c_write(REG_DAC31, 0x00);           /* DAC mute 关（后续 dac_start 再上电） */
    i2c_write(REG_DAC32, 0x00);           /* DAC 音量 0 */
    i2c_write(REG_ADC17, 0x00);           /* ADC 音量 0 */
    i2c_write(REG_SYS0E, 0xFF);           /* 全局电源关 */
    i2c_write(REG_SYS12, 0x02);           /* DAC 电源关 */
    i2c_write(REG_SYS14, 0x00);           /* PGA 关 */
    i2c_write(REG_SYS0D, 0xFA);           /* 全局电源关 */
    i2c_write(REG_ADC15, 0x00);
    i2c_write(REG_DAC37, 0x08);
    i2c_write(REG_SYS13, 0x10);           /* HP 驱动输出使能（官方 init L486） */
    i2c_write(REG_GP45, 0x01);

    LOG_I("codec init @ 0x%02x (mclk=%s)",
          s_addr, ES8311_MCLK_PIN < 0 ? "SCLK" : "MCLK");
    return 0;
}

void es8311_deinit(void)
{
    if (!s_inited) return;
    /* 轻量 suspend（保留 I2C 驱动，重 init 免重装） */
    i2c_write(REG_DAC32, 0x00);
    i2c_write(REG_ADC17, 0x00);
    i2c_write(REG_SYS0E, 0xFF);
    i2c_write(REG_SYS12, 0x02);
    i2c_write(REG_SYS14, 0x00);
    i2c_write(REG_SYS0D, 0xFA);
    i2c_write(REG_ADC15, 0x00);
    i2c_write(REG_DAC37, 0x08);
    i2c_write(REG_GP45, 0x01);
    /* 关时钟使能（2026-08-28）：时钟使能态下 MCLK 突停（MCU 复位，
     * 深睡唤醒/串口毛刺同款场景）会使内部时钟域挂死——之后 I2C
     * 持续 NACK 且总线电平健康，仅断电可解。失 MCLK 前先掉时钟
     * 使能，让芯片以静止态渡过无钟窗口。init/dac_start 重写 0x3F */
    i2c_write(REG_CLK_MGR01, 0x00);
    s_inited = false;
    LOG_I("codec deinit (suspend)");
}

int es8311_set_sample_rate(uint32_t rate)
{
    if (!s_inited) return -1;
    int c = find_coeff(rate);
    if (c < 0) {
        LOG_E("no coeff for %luHz (256×fs)", (unsigned long)rate);
        return -1;
    }
    const struct _coeff_div *d = &coeff_div[c];

    /* REG02: pre_div[7:5] / pre_multi[4:3] */
    uint8_t r02 = (uint8_t)(((d->pre_div - 1) & 0x07) << 5);
    uint8_t mul_bits = 0;
    switch (d->pre_multi) {
        case 1: mul_bits = 0; break;
        case 2: mul_bits = 1; break;
        case 4: mul_bits = 2; break;
        case 8: mul_bits = 3; break;
    }
    /* SCLK 作 mclk 源时强制 ×8（官方 v2.5 codec_init：DIG_MCLK =
     * LRCK×256 = BCLK×8；16bit 帧 BCLK=32×fs，表中 pre_multi 按
     * MCLK 脚直供假设取 1，不补偿则内部时钟仅 1/8 → DAC 解调乱码
     * 出气流声，2026-08-26 实测杂音定位） */
    if (ES8311_MCLK_PIN < 0) mul_bits = 3;
    r02 |= (uint8_t)((mul_bits & 0x03) << 3);
    i2c_write(REG_CLK_MGR02, r02);

    /* REG05: adc_div[7:4] / dac_div[3:0] */
    i2c_write(REG_CLK_MGR05,
              (uint8_t)(((d->adc_div - 1) << 4) | ((d->dac_div - 1) & 0x0F)));

    /* REG03: fs_mode[6] / adc_osr[5:0] */
    i2c_write(REG_CLK_MGR03,
              (uint8_t)(((d->fs_mode & 1) << 6) | (d->adc_osr & 0x3F)));

    /* REG04: dac_osr[5:0] */
    i2c_write(REG_CLK_MGR04, (uint8_t)(d->dac_osr & 0x3F));

    /* REG07/08: lrck 16 位 */
    i2c_write(REG_CLK_MGR07, (uint8_t)(d->lrck_h & 0xFF));
    i2c_write(REG_CLK_MGR08, (uint8_t)(d->lrck_l & 0xFF));

    /* REG06: bclk_div[4:0]（<19 时 -1，否则原值） */
    uint8_t bclk = d->bclk_div < 19 ? (uint8_t)(d->bclk_div - 1) : d->bclk_div;
    i2c_rmw(REG_CLK_MGR06, 0x1F, bclk & 0x1F);

    LOG_I("sample rate -> %luHz (coeff idx=%d)", (unsigned long)rate, c);
    return 0;
}

/* ---- DAC 起/停（含防爆破音时序） ---- */
void es8311_dac_start(void)
{
    if (!s_inited) return;
    /* 逐项转录 esp_codec_dev 1.5.6 es8311_start（2026-08-27 对齐：
     * REG00 保持 0x80 正常态非 0x00、补 REG14/15/17、REG37 0x08） */
    i2c_write(REG_RESET, 0x80);           /* 从机正常态（官方 start 终值） */
    i2c_write(REG_CLK_MGR01,              /* mclk 源重写（官方 start 同样重写） */
              (uint8_t)(0x3F | (ES8311_MCLK_PIN < 0 ? 0x80 : 0x00)));

    i2c_rmw(REG_SDPIN, 0x40, 0x00);       /* bit6=0 使能 DAC SDP */
    i2c_write(REG_ADC17, 0xBF);           /* 官方 start 无条件写（ADC 音量 0dB） */
    i2c_write(REG_SYS0E, 0x02);           /* 全局电源开 */
    i2c_write(REG_SYS12, 0x00);           /* DAC 电源开 */
    i2c_write(REG_SYS14, 0x1A);           /* 新增：AMIC 选通（官方固定值） */
    i2c_write(REG_SYS0D, 0x01);           /* 模拟电路上电（init 的 0xFA 由此恢复） */
    i2c_write(REG_ADC15, 0x40);           /* 新增：ADC ramp（官方固定值） */
    i2c_write(REG_DAC32, (uint8_t)((s_volume * 255) / 100));  /* 音量：驱动状态回写（默认75=0xBF，见 set_volume 注） */
    i2c_write(REG_DAC31, 0x00);           /* mute 关（最后一步） */
    i2c_write(REG_DAC37, 0x08);           /* 0x08 非 0x48：对齐 esp_codec_dev */
    i2c_write(REG_GP45, 0x00);
    LOG_D("dac start");
}

void es8311_dac_stop(void)
{
    if (!s_inited) return;
    /* 先 mute 再关电源（防爆破音） */
    i2c_write(REG_DAC31, 0x60);           /* soft mute */
    i2c_write(REG_SYS12, 0x02);           /* DAC 电源关 */
    i2c_rmw(REG_SDPIN, 0x40, 0x40);       /* bit6=1 关 DAC SDP */
    i2c_write(REG_DAC32, 0x00);
    i2c_write(REG_DAC37, 0x08);
    LOG_D("dac stop");
}

/* ---- ADC 起/停 ---- */
int es8311_adc_start(int gain_lvl)
{
    if (!s_inited) return -1;
    if (gain_lvl < 0 || gain_lvl > 7) {
        LOG_E("adc gain lvl %d out of range", gain_lvl);
        return -2;
    }

    /* 1. SDP 输出使能 */
    i2c_rmw(REG_SDPOUT, 0x40, 0x00);      /* bit6=0 使能 ADC SDP */

    /* 2. 电源链对齐官方 start：REG17/14/15 补齐，REG44 交由 init 的 0x58 */
    i2c_write(REG_ADC17, 0xBF);
    i2c_write(REG_SYS0E, 0x02);
    i2c_write(REG_SYS12, 0x00);
    i2c_write(REG_SYS14, 0x1A);           /* AMIC 选通（官方固定值） */
    i2c_write(REG_SYS0D, 0x01);
    i2c_write(REG_ADC15, 0x40);

    /* 3. PGA 增益（官方 esp_codec_dev：REG16 = 增益档 0~7） */
    i2c_write(REG_ADC16, (uint8_t)(gain_lvl & 0x07));

    i2c_write(REG_GP45, 0x00);
    LOG_D("adc start (gain=%d)", gain_lvl);
    return 0;
}

void es8311_adc_stop(void)
{
    if (!s_inited) return;
    i2c_write(REG_ADC17, 0x00);
    i2c_rmw(REG_SDPOUT, 0x40, 0x40);      /* bit6=1 关 ADC SDP */
    i2c_write(REG_SYS14, 0x00);           /* PGA 关 */
    LOG_D("adc stop");
}

int es8311_set_volume(int vol_0_100)
{
    if (!s_inited) return -1;
    if (vol_0_100 < 0) vol_0_100 = 0;
    if (vol_0_100 > 100) vol_0_100 = 100;
    s_volume = (uint8_t)vol_0_100;
    /* 0~100 → 0~255（REG32 线性） */
    uint8_t v = (uint8_t)((vol_0_100 * 255) / 100);
    i2c_write(REG_DAC32, v);
    return 0;
}

bool es8311_present(void)
{
    return s_inited && s_addr > 0;
}
