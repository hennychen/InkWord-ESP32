/**
 * @file es8311.c
 * @brief ES8311 codec 寄存器驱动实现 (2026-08-24)
 *
 * 移植自 esp-adf 官方 es8311 驱动（ESPRESSIF MIT License 2019），
 * 适配本项目：I2C 独占 GPIO38/39、MCLK 省线（REG01 bit7=1 选 SCLK
 * 作主时钟源，LyraT-Mini 同款）、精简 coeff 表（256×fs 覆盖 8k~48k）。
 *
 * 职责边界：本驱动只碰 I2C 寄存器；I2S 总线由 audio_player（DAC 侧）
 * 与 mic_recorder（ADC 侧）各自装卸，调用方在 I2S 重装后调
 * es8311_set_sample_rate 同步时钟系数。
 */
#include "es8311.h"
#include "gpio_config.h"
#include "debug_log.h"

#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"

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
#define REG_SYS13       0x13
#define REG_SYS14       0x14   /* PGA / DMIC */
#define REG_ADC15       0x15   /* ADC ramp / DMIC */
#define REG_ADC16       0x16   /* MIC PGA 增益 */
#define REG_ADC17       0x17   /* ADC 音量 */
#define REG_DAC31       0x31   /* DAC mute */
#define REG_DAC32       0x32   /* DAC 音量 */
#define REG_DAC37       0x37   /* DAC ramp */
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

/* ---- I2C 底层 ---- */
static int i2c_write(uint8_t reg, uint8_t val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (uint8_t)(s_addr << 1), 1);
    i2c_master_write_byte(cmd, reg, 1);
    i2c_master_write_byte(cmd, val, 1);
    i2c_master_stop(cmd);
    esp_err_t r = i2c_master_cmd_begin(ES8311_I2C_NUM, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return (r == ESP_OK) ? 0 : -1;
}

static int i2c_read(uint8_t reg, uint8_t *out)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (uint8_t)(s_addr << 1), 1);
    i2c_master_write_byte(cmd, reg, 1);
    i2c_master_stop(cmd);
    esp_err_t r = i2c_master_cmd_begin(ES8311_I2C_NUM, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    if (r != ESP_OK) return -1;

    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (uint8_t)((s_addr << 1) | 1), 1);
    i2c_master_read_byte(cmd, out, 0x01 /*NACK*/);
    i2c_master_stop(cmd);
    r = i2c_master_cmd_begin(ES8311_I2C_NUM, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return (r == ESP_OK) ? 0 : -1;
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

    /* 1. 装载 I2C 总线 */
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = ES8311_I2C_SDA_PIN,
        .scl_io_num = ES8311_I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = ES8311_I2C_FREQ_HZ,
    };
    if (i2c_param_config(ES8311_I2C_NUM, &cfg) != ESP_OK ||
        i2c_driver_install(ES8311_I2C_NUM, I2C_MODE_MASTER, 0, 0, 0) != ESP_OK) {
        LOG_E("i2c init failed (sda=%d scl=%d)",
              ES8311_I2C_SDA_PIN, ES8311_I2C_SCL_PIN);
        return -1;
    }

    /* 2. 探测 codec */
    if (es8311_probe() < 0) {
        LOG_E("codec not found on I2C (check module wiring)");
        i2c_driver_delete(ES8311_I2C_NUM);
        return -2;
    }

    /* 3. 复位 + 从机模式 + mclk 源选择 */
    i2c_write(REG_RESET, 0x80);           /* 软复位 */
    i2c_write(REG_SYS0B, 0x00);
    i2c_write(REG_SYS0C, 0x00);
    i2c_write(REG_SYS10, 0x1F);
    i2c_write(REG_SYS11, 0x7F);

    /* REG00 bit6: 1=master / 0=slave（本项目 I2S 主设备由 ESP32 承担，codec 从机） */
    i2c_write(REG_RESET, 0x00);

    /* REG01: bit7=mclk 源（0=MCLK 脚 / 1=SCLK 内部），bit5:4=时钟使能 */
    uint8_t reg01 = 0x3F;                 /* 使能全部时钟 */
    if (ES8311_MCLK_PIN < 0) {
        reg01 |= 0x80;                    /* 选 SCLK 作 mclk 源（省线） */
    }
    i2c_write(REG_CLK_MGR01, reg01);

    /* 4. 默认 44.1kHz 时钟系数 */
    if (es8311_set_sample_rate(44100) != 0) {
        LOG_W("default 44.1k coeff not found (non-fatal)");
    }

    /* 5. 默认静音态（DAC/ADC 均不上电） */
    i2c_write(REG_DAC31, 0x00);           /* DAC mute 关（后续 dac_start 再上电） */
    i2c_write(REG_DAC32, 0x00);           /* DAC 音量 0 */
    i2c_write(REG_ADC17, 0x00);           /* ADC 音量 0 */
    i2c_write(REG_SYS0E, 0xFF);           /* 全局电源关 */
    i2c_write(REG_SYS12, 0x02);           /* DAC 电源关 */
    i2c_write(REG_SYS14, 0x00);           /* PGA 关 */
    i2c_write(REG_SYS0D, 0xFA);           /* 全局电源关 */
    i2c_write(REG_ADC15, 0x00);
    i2c_write(REG_DAC37, 0x08);
    i2c_write(REG_GP45, 0x01);

    s_inited = true;
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
    /* 1. SDP 输出使能（先开数据通路，再上电，避免 pop） */
    i2c_rmw(REG_SDPIN, 0x40, 0x00);       /* bit6=0 使能 DAC SDP */

    /* 2. DAC 音量 + 电源 */
    i2c_write(REG_DAC32, 0xBF);           /* 音量 ~75% */
    i2c_write(REG_SYS0E, 0x02);           /* 全局电源开 */
    i2c_write(REG_SYS12, 0x00);           /* DAC 电源开 */

    /* 3. 解除 mute（最后一步） */
    i2c_write(REG_DAC31, 0x00);           /* mute 关 */
    i2c_write(REG_DAC37, 0x48);           /* ramp rate 起 */
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

    /* 2. 全局电源 + ADC 电源 */
    i2c_write(REG_SYS0E, 0x02);
    i2c_write(REG_SYS12, 0x00);

    /* 3. PGA 增益 + AMIC 选通（REG14 bit4:0 = PGA；bit6=0 关 DMIC） */
    i2c_write(REG_SYS14, (uint8_t)(0x1A | (gain_lvl & 0x07)));

    /* 4. ADC 音量 + ramp */
    i2c_write(REG_ADC17, 0xBF);
    i2c_write(REG_ADC15, 0x40);
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
    /* 0~100 → 0~255（REG32 线性） */
    uint8_t v = (uint8_t)((vol_0_100 * 255) / 100);
    i2c_write(REG_DAC32, v);
    return 0;
}

bool es8311_present(void)
{
    return s_inited && s_addr > 0;
}
