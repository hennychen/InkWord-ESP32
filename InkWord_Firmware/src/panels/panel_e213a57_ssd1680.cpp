/**
 * @file panel_e213a57_ssd1680.cpp
 * @brief E213A57 面板单元（L0）—— SSD1680 三色序列
 *
 * 面板：E213A57N203Q90，2.13" 122×250 黑白红三色，26P FPC
 * （手工改 24P 适配 EVK011 v1.4 转接板），3.3V。IC = SSD1680
 * （与 HINK-E0213A31 同控制器同分辨率，FPC 同源 26P）。
 *
 * bring-up（2026-09-09）：
 *   零轮：插屏后用 inkword-s3-hink213（BW 驱动）烧录，屏亮但
 *     背景红色——确认是三色屏（BW 驱动只写 0x24 B/W RAM，0x26
 *     Color RAM 未初始化，COG 默认态映射为红）。文字正常说明
 *     SSD1680 init 序列与 HINK-A31 兼容。
 *   一轮：HINK-A31 init 序列（同分辨率 SSD1680 已验证）
 *     + PSR 0x27（BWR 色序，REG LUT，同 WF0270 先例）+ 双平面
 *     写入（0x24 B/W + 0x26 red）+ 0x20 Master Activation 刷新。
 *   二轮（2026-09-10）：B/W 双色局刷启用——
 *     0x26 写旧帧 B/W + 0x24 写新帧 B/W + 0x20 Master Activation；
 *     红平面不写（COG RAM 保持上次全刷值，partial waveform 仅
 *     作用于 B/W 层）；实测局刷 ~10s（B/W 全屏差分），全刷 ~15s
 *     （三色波形），每 5 次局刷插 1 次全刷清残影。
 *
 * 与 HINK-E0213A31 区别（同 2.13" 同 SSD1680 但不同屏）：
 *   - 色彩：BW（单平面）vs BWR（双平面）；
 *   - PSR：HINK-A31 无显式 PSR（OTP 默认 BW）vs 本版 PSR 0x27；
 *   - 刷新：HINK-A31 用 0x22/0xF7 激活 vs 本版 0x20（三色标准）；
 *   - 行宽：均 16B/行（128px COG 原生，可见区 122px）。
 *
 * 与 WF0270 区别（同 SSD1680 三色但不同分辨率）：
 *   - 分辨率：128×250 vs 176×264；
 *   - Init：本版含 SWRESET/Driver Output/Border/0x21/0x18（HINK-A31
 *     同分辨率序列，WF0270 极简 OTP 预载路径）；
 *   - 时序：2.13" 小屏预计快于 WF0270 的 2.7"（待实测回填）。
 *
 * 序列来源：init = HINK-A31（GxEPD2 B74 忠实）+ PSR 0x27（WF0270）；
 * 刷新 = WF0270 双平面 + 0x20 Master Activation（SSD1680 三色标准）。
 *
 * 几何：COG 128×250 竖屏（panel_w=128，可见区 122px，右缘 6px
 * 无绑定源极），gfx_rotation=0 竖屏持机，LAYOUT_TINY 档。
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
extern const epd_panel_desc_t g_panel_e213a57;

static bool s_ready = false;   /* init 完成（power_off/deep_sleep 归零） */

/* COG 行宽 = 16 字节（128 位/行，SSD1680 原生宽度；
 * 可见区 122px，右缘 x122..127 六列无绑定源极不显示，
 * UI 排版需避开右缘 6px） */
#define K_ROW_BYTES 16

/* —— SSD1680 init（HINK-A31 GxEPD2 B74 忠实 + PSR 0x27 三色模式）—— */
static int panel_init(void)
{
    pinMode(EPD_RESET_PIN, OUTPUT);
    pinMode(EPD_CS_PIN, OUTPUT);
    pinMode(EPD_DC_PIN, OUTPUT);
    pinMode(EPD_BUSY_PIN, INPUT);
    digitalWrite(EPD_CS_PIN, HIGH);
    digitalWrite(EPD_DC_PIN, LOW);

    /* RST：HIGH 200ms → LOW rst_pulse_ms → HIGH 500ms */
    digitalWrite(EPD_RESET_PIN, HIGH);
    delay(200);
    digitalWrite(EPD_RESET_PIN, LOW);
    delay(g_panel_e213a57.rst_pulse_ms);
    digitalWrite(EPD_RESET_PIN, HIGH);
    delay(500);  /* COG boot 充分 */

    /* SWRESET：寄存器回 POR */
    bus_cmd(0x12);
    bus_wait_idle(&g_panel_e213a57, 5000);

    /* PSR：0x27 = REG LUT + BWR 色序（WF0270 同款——启用三色模式，
     * LUT 取 OTP 波形表，0x20 Master Activation 直接跑全刷序列） */
    bus_cmd(0x00);
    bus_dat(0x27);
    bus_dat(0x01);
    bus_dat(0x00);

    /* Driver Output Control：GD=0, SM=0, TB=0,
     * N+1=0x00F9=249 → 250 线扫描（HINK-A31 同分辨率同款） */
    bus_cmd(0x01);
    bus_dat(0xF9);
    bus_dat(0x00);
    bus_dat(0x00);

    /* Border Waveform Control：0x05 = VSS 边界 */
    bus_cmd(0x3C);
    bus_dat(0x05);

    /* Display Update Control 2：clock/analog 默认 + 温度传感器模式 */
    bus_cmd(0x21);
    bus_dat(0x00);
    bus_dat(0x80);

    /* Read Built-in Temperature Sensor：启用内部温度传感器 */
    bus_cmd(0x18);
    bus_dat(0x80);

    /* Data Entry Mode：0x03 = X+ Y+ */
    bus_cmd(0x11);
    bus_dat(0x03);

    /* RAM 窗口：X 字节 0..panel_w/8-1（128/8=16 → 0..15），
     * Y 0..panel_h-1（250 → 0..0x00F9） */
    bus_cmd(0x44);
    bus_dat(0x00);
    bus_dat((uint8_t)(g_panel_e213a57.panel_w / 8 - 1));
    bus_cmd(0x45);
    bus_dat(0x00); bus_dat(0x00);
    bus_dat((uint8_t)((g_panel_e213a57.panel_h - 1) & 0xFF));
    bus_dat((uint8_t)((g_panel_e213a57.panel_h - 1) >> 8));

    /* 地址计数器归零 */
    bus_cmd(0x4E); bus_dat(0x00);
    bus_cmd(0x4F); bus_dat(0x00); bus_dat(0x00);

    s_ready = true;
    return 0;
}

/* 双平面写入 + 全刷（WF0270 同款 SSD1680 三色标准）：
 * planes[0] = B/W 平面（bit=1 白）→ 0x24；
 * planes[1] = 红位平面（bit=1 红）→ 0x26；
 * 刷新直接 0x20 Master Activation（PSR 0x27 已含 LUT/色序配置） */
static int do_refresh(const uint8_t *bw_plane, const uint8_t *red_plane)
{
    if (!s_ready && panel_init() != 0) return -1;
    const size_t plane_bytes =
        (size_t)(g_panel_e213a57.panel_w / 8) * g_panel_e213a57.panel_h;

    /* Power-On（GxEPD2 _PowerOn 忠实：0x22/0xF8 → 0x20 → wait）
     * 首次 init 后 COG 已上电，power_on 幂等（BUSY 直接 LOW） */
    bus_cmd(0x22); bus_dat(0xF8);
    bus_cmd(0x20);
    bus_wait_busy(&g_panel_e213a57, g_panel_e213a57.power_on_ms);

    /* 地址计数器归零（无状态保证） */
    bus_cmd(0x4E); bus_dat(0x00);
    bus_cmd(0x4F); bus_dat(0x00); bus_dat(0x00);

    bus_cmd(0x24);
    bus_dat_stream(bw_plane, plane_bytes);
    bus_cmd(0x26);
    bus_dat_stream(red_plane, plane_bytes);

    bus_cmd(0x22);                    /* Display Update Control 2 */
    bus_dat(0xF7);                    /* 与 GxEPD2 _Update_Full/_Update_Part 一致 */
    bus_cmd(0x20);                    /* Master Activation（PSR 全配置） */
    bus_wait_busy(&g_panel_e213a57, g_panel_e213a57.busy_timeout_ms);
    return 0;
}

static int panel_full_refresh(const uint8_t *frame)
{
    /* frame：双平面连续布局：plane_count x panel_w/8 x panel_h 字节，
     * [0]=B/W 白位平面、[1]=红位平面 */
    const size_t plane_bytes =
        (size_t)(g_panel_e213a57.panel_w / 8) * g_panel_e213a57.panel_h;
    return do_refresh(frame, frame + plane_bytes);
}

static int panel_write_full(const uint8_t *frame)
{
    /* frame=NULL 清白：B/W RAM 全 0xFF + 红 RAM 全 0x00 + 0x20 */
    if (!frame) {
        if (!s_ready && panel_init() != 0) return -1;
        static const uint8_t white = 0xFF, no_red = 0x00;
        const size_t plane_bytes =
            (size_t)(g_panel_e213a57.panel_w / 8) * g_panel_e213a57.panel_h;
        bus_cmd(0x4E); bus_dat(0x00);
        bus_cmd(0x4F); bus_dat(0x00); bus_dat(0x00);
        bus_cmd(0x24);
        for (size_t i = 0; i < plane_bytes; i++) bus_dat(white);
        bus_cmd(0x26);
        for (size_t i = 0; i < plane_bytes; i++) bus_dat(no_red);
        bus_cmd(0x22); bus_dat(0xF7);   /* Display Update Control 2 */
        bus_cmd(0x20);
        bus_wait_busy(&g_panel_e213a57, g_panel_e213a57.busy_timeout_ms);
        return 0;
    }
    return panel_full_refresh(frame);
}

static int panel_write_planes(const uint8_t *const *planes)
{
    /* 多平面统一入口（§9.3）：planes[] 指针数组 */
    return do_refresh(planes[0], planes[1]);
}

/* 显示寄存器重配（GxEPD2 _InitDisplay 忠实）：
 * 每次局刷前调用，重配 RAM 窗口/温度/Boundary 等（不含 SWRESET/PSR） */
static void reinit_display(void)
{
    /* Driver Output Control：GD=0, SM=0, TB=0, N+1=0x00F9=249 → 250 线 */
    bus_cmd(0x01);
    bus_dat(0xF9); bus_dat(0x00); bus_dat(0x00);
    /* Data Entry Mode：0x03 = X+ Y+ */
    bus_cmd(0x11); bus_dat(0x03);
    /* Border Waveform Control：0x05 = VSS */
    bus_cmd(0x3C); bus_dat(0x05);
    /* Read Built-in Temperature Sensor */
    bus_cmd(0x18); bus_dat(0x80);
    /* Display Update Control 2：clock/analog 默认 + 温度传感器模式 */
    bus_cmd(0x21); bus_dat(0x00); bus_dat(0x80);
    /* RAM 窗口：X 字节 0..panel_w/8-1，Y 0..panel_h-1 */
    bus_cmd(0x44);
    bus_dat(0x00);
    bus_dat((uint8_t)(g_panel_e213a57.panel_w / 8 - 1));
    bus_cmd(0x45);
    bus_dat(0x00); bus_dat(0x00);
    bus_dat((uint8_t)((g_panel_e213a57.panel_h - 1) & 0xFF));
    bus_dat((uint8_t)((g_panel_e213a57.panel_h - 1) >> 8));
}

/* B/W 双色局刷（SSD1680 三色差分驱动）：
 * 0x26 写旧帧 B/W（差分基准）+ 0x24 写新帧 B/W（当前帧）；
 * 红平面不写（COG RAM 保持上次全刷值，partial waveform 仅作用于
 * B/W 层——GDEY0213Z98 官方规格：局刷 1.8s 窗口 / ~10s 全屏 B/W）；
 * 0x20 Master Activation 触发（PSR 0x27 OTP 自动选 B/W 差分波形）。
 * 架构与 HINK-A31 同族（0x26/0x24 双 RAM 差分），但激活用 0x20
 * 而非 0x22/0xF7（三色 PSR 0x27 配置路径） */
static int panel_partial(const uint8_t *prev, const uint8_t *new_,
                         uint8_t passes)
{
    if (passes < 1) passes = 1;
    if (!s_ready && panel_init() != 0) return -1;

    const size_t plane_bytes =
        (size_t)(g_panel_e213a57.panel_w / 8) * g_panel_e213a57.panel_h;

    #if INKWORD_EPD_DIAG
    const uint32_t t0 = millis();
    #endif

    for (uint8_t p = 0; p < passes; p++) {
        /* Power-On（GxEPD2 _PowerOn 忠实：0x22/0xF8 → 0x20 → wait）
         * 上次全刷/局刷后 COG 已关电，必须先唤醒才能接受新命令 */
        bus_cmd(0x22); bus_dat(0xF8);
        bus_cmd(0x20);
        bus_wait_busy(&g_panel_e213a57, g_panel_e213a57.power_on_ms);

        reinit_display();                               /* 重配寄存器（GxEPD2 _InitDisplay 等价） */
        /* 地址计数器归零（无状态保证） */
        bus_cmd(0x4E); bus_dat(0x00);
        bus_cmd(0x4F); bus_dat(0x00); bus_dat(0x00);

        bus_cmd(0x26);                                    /* Previous B/W（旧帧差分基准） */
        bus_dat_stream(prev, plane_bytes);
        bus_cmd(0x24);                                    /* Current B/W（新帧） */
        bus_dat_stream(new_, plane_bytes);
        /* 红平面不写：COG RAM 0x26 高位保持上次全刷值，
         * partial waveform 仅驱动 B/W 层差分 */

        bus_cmd(0x22);                                    /* Display Update Control 2 */
        bus_dat(0xF7);                                    /* 与 GxEPD2 _Update_Part 一致 */
        bus_cmd(0x20);                                    /* Master Activation */
        bus_wait_busy(&g_panel_e213a57, g_panel_e213a57.busy_timeout_ms);
    }

    DIAG_LOG("partial %ux took %ums",
             passes, (unsigned)(millis() - t0));
    return 0;
}

static void panel_power_off(void)
{
    /* P2d：SSD16xx 族关电收敛 epd_bus */
    bus_ssd16_power_off(&g_panel_e213a57, &s_ready);
}

static void panel_deep_sleep(void)
{
    bus_ssd16_deep_sleep(&s_ready);
}

/* —— desc 注册（E213A57，SSD1680 三色，LAYOUT_TINY 档）——
 * 时序：同分辨率 HINK-A31 实测 ~3.9s 为 BW 基线，三色波形预计
 * 更长（待 bring-up 实测回填）；BUSY 极性 HIGH=忙（SSD16xx 标准）；
 * plane 语义 0x24 bit=1 白 / 0x26 bit=1 红（WF0270 同源） */
const epd_panel_desc_t g_panel_e213a57 = {
    .name       = "e213a57_ssd1680",
    .controller = EPD_CTRL_SSD1680,
    .panel_w    = 128,            /* COG RAM 宽度（128px = 16B/行；
                                   * 可见区 122px，右缘 6px 无绑定） */
    .panel_h    = 250,
    .gfx_rotation = 0,            /* 竖屏持机（gfx 128x250，LAYOUT_TINY；
                                   * 可见区 122x250，UI 排版避开右缘 6px） */
    .dpi         = 130,           /* 对角 PPI（同 HINK-A31，2.13" 对角 278px） */
    .color_mode = EPD_COLOR_3C,   /* 黑白红三色 */
    .plane_count = 2,             /* B/W 白位平面 + 红位平面 */
    .palette    = {
        /* 逻辑色 → 平面置位掩码：bit0=B/W 平面白位，bit1=红平面红位
         * （三色映射与 WF0270/E042A13 一致，SSD16xx 家族 RAM 语义同源） */
        [EPD_GFX_WHITE]  = 0x01,   /* (bw=1,red=0) → 白 */
        [EPD_GFX_BLACK]  = 0x00,   /* (0,0) → 黑 */
        [EPD_GFX_ACCENT] = 0x02,   /* (0,1) → 红 */
        [EPD_GFX_AUX]    = 0x00,   /* 3C 无第四色 → 退化黑 */
    },
    .accent_rgb = 0xFF0000,    /* 红（LAN 量化调色板注入） */
    .fb_location = EPD_FB_AUTO,   /* 双帧+画布 ~24KB << 128KB → 全 SRAM */
    .rst_pulse_ms = 10,           /* RST LOW 脉宽（SSD16xx 标准 10ms） */
    .busy_level = 1,              /* BUSY=HIGH 忙（SSD16xx 系标准） */
    .busy_timeout_ms = 15000,     /* 三色全刷 ~15s + 余量 */
    .power_on_ms  = 100,
    .power_off_ms = 150,
    .full_ms    = 15000,          /* 三色全刷实测 ~15s（2.13" 小屏三色
                                   * 波形，Good Display 规格 15s Fast） */
    .partial_ms = 15000,          /* 差分局刷 = 全刷（GxEPD2 实测 14.6s；
                                   * SSD1680 OTP 无独立快刷波形，全/局同速） */
    .partial_enabled = false,     /* 三色屏差分局刷残影严重（红平面不更新 +
                                   * B/W 差分波形非 OTP 优化），统一走全刷；
                                   * 2026-09-10 真机验证后关闭 */
    .passes     = 1,
    .partial_count_full_refresh = 5, /* 每 5 次局刷插 1 次全刷清残影
                                   *（三色 partial 无红通道更新，残影
                                   * 累积比 BW 快，5 次保守周期） */
    .window_8align = true,        /* 0x44 窗口 x 8 像素对齐（128/8=16 ✓） */
    .ops = {
        .init         = panel_init,
        .full_refresh = panel_full_refresh,
        .write_full   = panel_write_full,
        .partial      = panel_partial,  /* B/W 双色差分局刷 */
        .power_off    = panel_power_off,
        .deep_sleep   = panel_deep_sleep,
        .probe        = NULL,     /* SSD1680 0x2F 版本读可作 probe */
        .write_planes = panel_write_planes,
        .diag         = bus_diag_ssd16, /* T1.8：SSD16xx 0x2F 双读 */
    },
};
