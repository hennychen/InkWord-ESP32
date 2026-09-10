/**
 * @file panel_hink_e0213a31.cpp
 * @brief HINK-E0213A31 面板单元（L0）—— SSD1680 序列（GxEPD2 B74 路线）
 *
 * 面板：HINK-E0213A31-A0（FPC 标 HINK-E0213A31-A0，2019-11 产），
 * 2.13" 黑白电子墨水屏，26P FPC（34P 宽度，有效引脚 26），3.3V。
 * 与 Good Display GDEM0213B74 / GDEY0213B74 同规格（122×250 可见，
 * COG 128×250，SSD1680 驱动 IC）。
 *
 * 序列来源（权威，GxEPD2 GxEPD2_213_B74 忠实移植）：
 *   init  = RST 200/10/200 + SWRESET(0x12) + Driver Output(0x01: 0xF9/0/0)
 *           + Border(0x3C: 0x05) + Display Update(0x21: 0/0x80)
 *           + Temp Sensor(0x18: 0x80) + RAM 窗口/计数器；
 *   刷新  = 0x26 写旧帧 + 0x24 写新帧（双写差分）→ 0x22/0xF7 → 0x20；
 *   关电  = 0x22/0x83 → 0x20（SSD16xx 族标准）；
 *   深睡  = 0x10/0x01（RST 硬复位唤醒）。
 *
 * P0 探针实测结论（2026-09-05 终局）：
 *   0xF7 标准全刷 = 3861ms，稳定可用（唯一可用方案）；
 *   0xD7 快速全刷 = 2314ms（bitbang 探针通过），但 HW SPI 正式驱动连续刷新
 *   导致纯黑屏，不可用（推测 LUT 波形与本屏 COG OTP 不兼容）；
 *   0xFC 局刷 = 3379ms ≈ 全刷，无加速（OTP 无独立快刷波形）；
 *   窗口局刷 64x64 = 3380ms，SSD1680 仍驱动全屏，无加速；
 *   → 全刷/局刷路径统一使用 0xF7 标准全刷。
 *
 * 几何：COG 物理 128×250 竖屏（panel_w=128 = RAM 行宽 16B，
 * 可见区 122px，右缘 6px 无绑定源极不显示；gfx_rotation=0 竖屏持机，
 * LAYOUT_TINY 档）。stride = (128+7)/8 = 16B/行，帧 = 4000B。
 *
 * 与 OPM021EB 区别（同 2.13" 但不同屏）：
 *   - 控制器：SSD1680（BUSY=HIGH 忙）vs UC8151D（BUSY=LOW 忙）；
 *   - 行宽：16B/行（128px COG 原生）vs 15B/行（122px 非 8 整除特例）；
 *   - 序列：SSD16xx 标准（0x24/0x26/0x20）vs UC8151D（0x10/0x13/0x12）；
 *   - 右缘：6px 无绑定（COG 128 绑 122）vs 2px 丢失（15B 截断）。
 *
 * 无状态设计（desc 头注释铁律）：s_ready 前置检查，power_off/
 * deep_sleep 后归零，下次刷新 RST 唤醒 + 完整重配。
 */
#include "../epd_panel.h"
#include "../gpio_config.h"
#include "epd_bus.h"    /* T1.1：SPI 原语/等待/判活收敛层 */

#include <Arduino.h>
#include <SPI.h>

/* 前置声明：ops 实现引用 desc 几何/时序字段 */
extern const epd_panel_desc_t g_panel_hink_e0213a31;

static bool s_ready = false;   /* init 完成（power_off/deep_sleep 归零） */

/* COG 行宽 = 16 字节（128 位/行，SSD1680 原生宽度；
 * 可见区 122px，右缘 x122..127 六列无绑定源极不显示，
 * UI 排版需避开右缘 6px */
#define K_ROW_BYTES 16

/* —— SSD1680 init（GxEPD2 GxEPD2_213_B74._InitDisplay 忠实移植） —— */
static int panel_init(void)
{
    /* HINK-E0213A31 实测：COG boot 自检脉冲极短（或不触发），
     * bus_detect_alive() 要求 BUSY 忙→闲往返会误判 fail。
     * 改为直接 RST 脉冲 + 长等 boot，不依赖 BUSY 判活。
     * （强制刷新探针证实：init + refresh 全序列正常，BUSY 在
     * 0x20 激活后正确 HIGH→3450ms→LOW，SPI 通信完好） */
    pinMode(EPD_RESET_PIN, OUTPUT);
    pinMode(EPD_CS_PIN, OUTPUT);
    pinMode(EPD_DC_PIN, OUTPUT);
    pinMode(EPD_BUSY_PIN, INPUT);
    digitalWrite(EPD_CS_PIN, HIGH);
    digitalWrite(EPD_DC_PIN, LOW);

    /* RST：HIGH 200ms → LOW rst_pulse_ms → HIGH 500ms
     * （SSD16xx 标准，VCI 升压充分再 boot） */
    digitalWrite(EPD_RESET_PIN, HIGH);
    delay(200);
    digitalWrite(EPD_RESET_PIN, LOW);
    delay(g_panel_hink_e0213a31.rst_pulse_ms);
    digitalWrite(EPD_RESET_PIN, HIGH);
    delay(500);  /* COG boot 充分（实测 ~50ms 即可，取 500ms 保守） */

    /* SWRESET：寄存器回 POR（GxEPD2 _InitDisplay 第一步） */
    bus_cmd(0x12);
    bus_wait_idle(&g_panel_hink_e0213a31, 5000);

    /* Driver Output Control（0x01）：GD=0, SM=0, TB=0,
     * N+1=0x00F9=249 → 250 线扫描（GxEPD2 B74 同款；
     * GDEY0213B74 变体亦用 0xF9，COG 统一 250 线配置） */
    bus_cmd(0x01);
    bus_dat(0xF9);
    bus_dat(0x00);
    bus_dat(0x00);

    /* Border Waveform Control（0x3C）：0x05 = VSS 边界
     * （GxEPD2 B74 同款；防止边界闪白） */
    bus_cmd(0x3C);
    bus_dat(0x05);

    /* Display Update Control 2（0x21）：0x00, 0x80
     * （GxEPD2 B74 同款；clock/analog 默认 + 温度传感器模式） */
    bus_cmd(0x21);
    bus_dat(0x00);
    bus_dat(0x80);

    /* Read Built-in Temperature Sensor（0x18）：0x80
     * （GxEPD2 B74 同款；启用内部温度传感器） */
    bus_cmd(0x18);
    bus_dat(0x80);

    /* Data Entry Mode（0x11）：0x03 = X+ Y+（GxEPD2 B74 同款；
     * GDEY0213B74 变体用 0x01，本屏 FPC 标 A0 对应 B74 序列） */
    bus_cmd(0x11);
    bus_dat(0x03);

    /* RAM 窗口：X 字节 0..panel_w/8-1（128/8=16 → 0..15），
     * Y 0..panel_h-1（250 → 0..0x00F9） */
    bus_cmd(0x44);
    bus_dat(0x00);
    bus_dat((uint8_t)(g_panel_hink_e0213a31.panel_w / 8 - 1));
    bus_cmd(0x45);
    bus_dat(0x00); bus_dat(0x00);
    bus_dat((uint8_t)((g_panel_hink_e0213a31.panel_h - 1) & 0xFF));
    bus_dat((uint8_t)((g_panel_hink_e0213a31.panel_h - 1) >> 8));

    /* 地址计数器归零 */
    bus_cmd(0x4E); bus_dat(0x00);
    bus_cmd(0x4F); bus_dat(0x00); bus_dat(0x00);

    s_ready = true;
    return 0;
}

/* RAM 整帧写入（SSD1680 标准 16B/行，单 CS 事务流）：
 * frame=NULL 时同事务全量写白（0xFF） */
static void write_ram_frame(const uint8_t *frame)
{
    const size_t ram_bytes =
        (size_t)K_ROW_BYTES * g_panel_hink_e0213a31.panel_h;
    bus_dat_stream(frame, ram_bytes);
}

/* 双写差分 + 快速全刷（SSD1680 差分语义 + GDEY0213B74 快速模式）：
 * 0x26 写旧帧 + 0x24 写新帧 → 0x1A/0x64（温度寄存器 25°C）
 * → 0x22/0xD7 → 0x20。
 * 实测时序（P0 探针 2026-09-05）：
 *   标准全刷 0xF7 = 3460ms；快速全刷 0xD7 = 1911ms（省 45%）；
 *   局刷 0xFC = 3379ms（≈全刷，OTP 无快刷波形，0xFC 无加速效果）；
 *   窗口局刷 64x64 = 3380ms（SSD1680 仍驱动全屏，窗口无加速）。
 * 结论：0xD7 快速全刷是唯一有效加速方案，统一用于全刷和局刷路径。
 * 双写是 SSD16xx 家族差分驱动语义（单写屏显恒滞后一帧） */
static int do_refresh(const uint8_t *frame)
{
    if (!s_ready && panel_init() != 0) return -1;

    /* 地址计数器归零（无状态保证） */
    bus_cmd(0x4E); bus_dat(0x00);
    bus_cmd(0x4F); bus_dat(0x00); bus_dat(0x00);

    bus_cmd(0x26);                     /* Previous RAM（旧帧基准） */
    write_ram_frame(frame);
    bus_cmd(0x24);                     /* Current RAM（新帧） */
    write_ram_frame(frame);

    /* 标准全刷序列：0x22/0xF7 → 0x20
     * （0xD7 快速全刷在本屏正式驱动中导致纯黑屏，探针 bitbang 虽通过
     * 但 HW SPI 连续刷新不兼容；0xF7 = 3861ms 稳定可靠；
     * 0xF5 Partial-Update 测试：能显示但速度相同 ~3.5s，OTP 无独立快刷波形） */
    bus_cmd(0x22);
    bus_dat(0xF7);
    bus_cmd(0x20);
    bus_wait_busy(&g_panel_hink_e0213a31,
                  g_panel_hink_e0213a31.busy_timeout_ms);
    return 0;
}

static int panel_full_refresh(const uint8_t *frame)
{
    /* frame：BW 单平面整帧（bit=1 白），16B/行 × 250 行 = 4000B */
    return do_refresh(frame);
}

static int panel_write_full(const uint8_t *frame)
{
    /* 竖屏原始帧直通全刷（epd_full_refresh / LAN 语义）：frame=NULL
     * 清白（SSD1680 快速模式：双 RAM 全 0xFF + 0x1A/0x64 + 0x22/0xD7 → 0x20）。
     * 注：本路径不维护 prev 帧一致性，调用方须强制下一次全刷
     * （ui_force_full_refresh_next 已保证） */
    if (!frame) {
        if (!s_ready && panel_init() != 0) return -1;
        const size_t ram_bytes =
            (size_t)K_ROW_BYTES * g_panel_hink_e0213a31.panel_h;
        bus_cmd(0x4E); bus_dat(0x00);
        bus_cmd(0x4F); bus_dat(0x00); bus_dat(0x00);
        bus_cmd(0x26);
        for (size_t i = 0; i < ram_bytes; i++) bus_dat(0xFF);
        bus_cmd(0x24);
        for (size_t i = 0; i < ram_bytes; i++) bus_dat(0xFF);
        bus_cmd(0x22); bus_dat(0xF7);    /* 标准全刷 */
        bus_cmd(0x20);
        bus_wait_busy(&g_panel_hink_e0213a31,
                      g_panel_hink_e0213a31.busy_timeout_ms);
        return 0;
    }
    return panel_full_refresh(frame);
}

/* 局刷（SSD1680 差分驱动 + 快速全刷激活）：
 * 实测 0x22/0xFC 局刷 = 3379ms ≈ 全刷 3460ms（OTP 无快刷波形），
 * 0x22/0xD7 快速全刷 = 1911ms（省 45%）。
 * 统一使用 0xD7 快速激活：双写差分 + 0x1A/0x64 + 0x22/0xD7 → 0x20。
 * prev 不参与（差分由 COG 0x26/0x24 双 RAM 承担） */
static int panel_partial(const uint8_t *prev, const uint8_t *new_,
                         uint8_t passes)
{
    (void)prev;
    if (passes < 1) passes = 1;
    if (!s_ready && panel_init() != 0) return -1;

    #if INKWORD_EPD_DIAG
    const uint32_t t0 = millis();
    #endif
    for (uint8_t p = 0; p < passes; p++) {
        bus_cmd(0x4E); bus_dat(0x00);
        bus_cmd(0x4F); bus_dat(0x00); bus_dat(0x00);
        bus_cmd(0x26);
        write_ram_frame(new_);
        bus_cmd(0x24);
        write_ram_frame(new_);
        /* 标准全刷激活（0xD7 在本屏 HW SPI 下黑屏，回退 0xF7） */
        bus_cmd(0x22);
        bus_dat(0xF7);
        bus_cmd(0x20);
        bus_wait_busy(&g_panel_hink_e0213a31,
                      g_panel_hink_e0213a31.busy_timeout_ms);
    }
    DIAG_LOG("partial %ux took %ums",
             passes, (unsigned)(millis() - t0));
    return 0;
}

static void panel_power_off(void)
{
    /* P2d：SSD16xx 族关电收敛 epd_bus（0x22/0x83 + 0x20） */
    bus_ssd16_power_off(&g_panel_hink_e0213a31, &s_ready);
}

static void panel_deep_sleep(void)
{
    bus_ssd16_deep_sleep(&s_ready);
}

/* —— desc 注册（HINK-E0213A31，SSD1680，LAYOUT_TINY 档）——
 * 时序：P0 探针实测回填（2026-09-05）；BUSY 极性 HIGH=忙（SSD16xx 标准）；
 * 行宽 16B/行（COG 128px 原生，无 OPM021EB 的 15B 截断问题）。
 *
 * 实测时序：
 *   标准全刷 0xF7 = 3861ms；快速全刷 0xD7 = 2314ms（省 40%）；
 *   局刷 0xFC = 3379ms（≈全刷，OTP 无快刷波形）；
 *   窗口局刷 64x64 = 3380ms（SSD1680 仍驱动全屏，窗口无加速）。
 *   → 统一使用 0xD7 快速全刷（全刷/局刷路径均用） */
const epd_panel_desc_t g_panel_hink_e0213a31 = {
    .name       = "hink_e0213a31_bw",
    .controller = EPD_CTRL_SSD1680,
    .otp_signature = 0,           /* 待实测指纹（自动识别暂不启用） */
    .panel_w    = 128,            /* COG RAM 宽度（128px = 16B/行；
                                   * 可见区 122px，右缘 6px 无绑定） */
    .panel_h    = 250,
    .gfx_rotation = 0,            /* 竖屏持机（gfx 128x250，LAYOUT_TINY；
                                   * 可见区 122x250，UI 排版避开右缘 6px） */
    .dpi         = 130,           /* 对角 PPI（诊断字段：2.13" 对角 278px；
                                   * 换算 mm=px÷PPI×25.4） */
    .color_mode = EPD_COLOR_BW,
    .plane_count = 1,             /* BW 单平面 */
    .palette    = {
        /* BW 语义（bit=1 白 / bit=0 黑，SSD1680 0x24 标准） */
        [EPD_GFX_WHITE]  = 0x01,
        [EPD_GFX_BLACK]  = 0x00,
        [EPD_GFX_ACCENT] = 0x00,  /* BW 退化：强调色降级黑 */
        [EPD_GFX_AUX]    = 0x00,
    },
    .accent_rgb = 0x000000,       /* BW 无第三色，LAN 调色板不注入 */
    .fb_location = EPD_FB_AUTO,   /* 帧 4000B x2 + 画布 4000B ≈ 12KB
                                   * 全 SRAM（§10.1 余量最宽） */
    .rst_pulse_ms = 10,           /* RST LOW 脉宽（SSD16xx 标准 10ms） */
    .busy_level = 1,              /* BUSY=HIGH 忙（SSD16xx 系标准） */
    .busy_timeout_ms = 10000,     /* 全刷 ~3.6s + 余量（GxEPD2 标称） */
    .power_on_ms  = 100,          /* GxEPD2 power_on_time=100ms */
    .power_off_ms = 150,          /* GxEPD2 power_off_time=150ms */
    .full_ms    = 4000,           /* P0+残影探针实测：标准全刷 0xF7 = 3861ms
                                   *（0xD7 快速全刷 HW SPI 下黑屏，不可用） */
    .partial_ms = 4000,           /* P0 实测：0xFC = 3379ms ≈ 全刷，
                                   * 统一用 0xF7 标准全刷 = 3861ms
                                   *（OTP 无快刷波形，0xFC/0xD7 均不可用） */
    .partial_enabled = true,      /* 双 RAM 差分局刷（0x26/0x24 双写）；
                                   * 激活用 0xD7 快速全刷（0xFC 无加速） */
    .passes     = 1,
    .partial_count_full_refresh = 8, /* 局刷计数保养（SSD1680 快刷
                                   * 残影较轻，8 次保守；bring-up 实测
                                   * 后校准） */
    .window_8align = true,        /* 0x44 窗口 x 8 像素对齐（128/8=16 ✓） */
    .ops = {
        .init         = panel_init,
        .full_refresh = panel_full_refresh,
        .write_full   = panel_write_full,
        .partial      = panel_partial,     /* SSD1680 寄存器级快刷 */
        .power_off    = panel_power_off,
        .deep_sleep   = panel_deep_sleep,
        .probe        = NULL,              /* SSD1680 0x2F 版本读可作 probe
                                            * （epd_driver 诊断路径已覆盖） */
        .write_planes = NULL,              /* BW 单平面，无多平面入口 */
        .diag         = bus_diag_ssd16,    /* T1.8：SSD16xx 0x2F 双读 */
    },
};
