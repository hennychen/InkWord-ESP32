/**
 * @file panel_e042a13_ssd1619.cpp
 * @brief Hink E042A13-A0 面板单元（L0）—— SSD1619 手写序列 + 三色 desc
 *        （PANEL_COMPAT_DESIGN §3.2 第二块屏 / §十六 SOP 首个色彩面板）
 *
 * 面板：Hink E042A13-A0（丝印 HE042A83A1 17A033-PLA NBLG0P05，
 * Date 2017-4-24 SYX 2118）4.2" 400x300 黑白红三色，24P FPC。
 * !! IC 实为 SSD1619（用户 2026-08-22 确认，GDEH042Z96 同族）——
 * 初版误判 IL0398（GDEW042Z15 同族）导致全序列落空：0x10/0x13 写
 * RAM SSD1619 不认（正确 0x24/0x26）、BUSY 极性反（SSD16xx 系
 * HIGH=忙）、0x12 直接激活对 SSD1619 需走 0x22 序列。真机症状：
 * BUSY 真驱动（在场）但 0x12 后从不忙、屏无变化。
 *
 * 序列来源（权威，一比一移植）：Waveshare 4.2inch e-Paper (B) V2
 * 官方 demo epd4in2b_V2.cpp（Init_new/Display_new/Clear_new/Sleep_new，
 * 同为 SSD1619 400x300 BWR）+ GDEH042Z96 官方规格书 §4.1 初始代码
 * （0x74/0x7E/0x0C/0x2B/0x01/0x3A/0x3B，2017 批次显式下发）。GxEPD2
 * 1.6.9 无 Z96 类，故绕开 GxEPD2 面板类手写 SPI 序列（BUSY 极性 /
 * 时序完全自控）。
 *
 * bring-up 完成（2026-08-22 真机，三根因均实证修复）：① IC 判错
 * IL0398→SSD1619（本文件重建）；② ESP32-S3 SPI.writeBytes 批量写
 * RAM 不落地（恒定全红），改 transfer 逐字节连发；③ 上层
 * ac_layer_color 条件颠倒致 red plane 恒全置（epd_driver.cpp 同日
 * 勘误）。终态：待机页文字正常显示，全刷 14580ms 稳定复现。
 *
 * 快刷实验结论（2026-08-22，勘误 2026-09-01）：同族 SSD1683 的
 * 黑白快刷位组合 0x22/0xDC 在本屏 0x20 后 BUSY 仅 259ms 空转（无
 * 波形输出）→ 当时判「SSD1619 OTP 无差分快刷波形路径，14.6s
 * 全刷即本屏下限」。勘误：空转根因是 0x10 位要求载入 LUT 寄存器
 * 而 0x32 从未写入（空 LUT=零帧波形），并非芯片无能力——0x32
 * 寄存器 LUT 差分局刷路径已由黑白兄弟屏 panel_e042a13bw.cpp
 * v8 定稿（2026-08-30 真机 v1-v8 八轮迭代）打通，本文件
 * 2026-09-01 同构移植（三色屏 BW-only 用途，红需求仍走全刷）。
 * 外部交叉验证：EPaperDrive 库 DKE42_3COLOR（SSD1619 4.2" 三色）
 * 同路径局刷 + SSD1619A spec §6.7 0x32 七十字节 WS 格式。
 *
 * 双 RAM 语义（SSD16xx）：0x24 B/W RAM bit=1 白 + 0x26 color RAM
 * bit=1 红（Waveshare Clear_new 黑 0xFF/红 0x00 = 白屏；Display_new
 * 红 RAM 写 ~k 因其 image 约定 bit=0=红，本单元 plane[1] 约定
 * bit=1=红直通，与 GxEPD2 GDEY042Z98 0x26 语义一致）。
 *
 * 无状态设计（desc 头注释铁律）：do_refresh 前置 s_ready 检查，
 * power_off/deep_sleep 后归零，下次刷新自动 RST 唤醒 + 完整重配
 * （Waveshare Sleep 注释实证：RST 唤醒 + Init 重初始化）。
 */
#include "../epd_panel.h"
#include "../gpio_config.h"
#include "epd_bus.h"    /* T1.1：SPI 原语/等待收敛层 */

#include <Arduino.h>
#include <SPI.h>
#include "../debug_log.h"   /* LOG_I：局刷 DIAG 统一 ESP_LOG 通道（2026-09-01：
                             * Serial.printf 与 ESP_LOG 无互斥并发写 UART，
                             * 行交错/截断/乱序致归属不可判，A3 轮实锤）。
                             * !! 铁律（debug_log.h 头注释）：必须在 Arduino.h
                             * 之后 include，否则 esp32-hal-log 重新劫持
                             * ESP_LOGx 为 CORE_DEBUG_LEVEL=NONE 空宏（A4 轮
                             * 首烧实证：全屏刷照跑而 DIAG 全静默） */
#include <string.h>    /* memcmp：局刷 diff 脏区检测（BW 兄弟屏同构） */

/* 前置声明：ops 实现引用 desc 几何/时序字段 */
extern const epd_panel_desc_t g_panel_e042a13;

static const char *TAG = "E042";   /* LOG_I TAG（debug_log.h 约定） */

static bool s_ready = false;   /* init 完成（power_off/deep_sleep 归零） */


static int panel_init(void)
{
    /* GDEH042Z96 官方规格书 §4.1 Typical Operating Sequence 一比一
     * （2026-08-22 全红勘误：Waveshare 4in2b_V2 demo 仅适用于其 2020
     * 批次（OTP 预载默认），本屏 2017 批次须显式下发完整初始代码 ——
     * 缺 0x0C softstart / 0x2B ACVCOM / 0x74/0x7E 块控制时升压与
     * VCOM 跑偏，波形照跑但整屏纯色（真机实测全红）） */
    pinMode(EPD_RESET_PIN, OUTPUT);
    pinMode(EPD_CS_PIN, OUTPUT);
    pinMode(EPD_DC_PIN, OUTPUT);
    pinMode(EPD_BUSY_PIN, INPUT);
    digitalWrite(EPD_CS_PIN, HIGH);
    digitalWrite(EPD_DC_PIN, LOW);

    digitalWrite(EPD_RESET_PIN, HIGH);
    delay(10);
    digitalWrite(EPD_RESET_PIN, LOW);
    delay(g_panel_e042a13.rst_pulse_ms);
    digitalWrite(EPD_RESET_PIN, HIGH);
    delay(10);
    bus_wait_idle(&g_panel_e042a13, 5000);            /* 复位后 boot 自检（忙→闲） */

    bus_cmd(0x12);                    /* SWRESET：寄存器回 POR，VCOM 载 OTP */
    bus_wait_idle(&g_panel_e042a13, 5000);

    /* —— 官方初始代码（§4.1 步骤 3，参数取命令表标定值）—— */
    bus_cmd(0x74); bus_dat(0x54);     /* Set Analog Block Control */
    bus_cmd(0x7E); bus_dat(0x3B);     /* Set Digital Block Control */
    bus_cmd(0x0C);                    /* Softstart：四段软启动 */
    bus_dat(0x8E); bus_dat(0x8C); bus_dat(0x85); bus_dat(0x3F);
    bus_cmd(0x2B); bus_dat(0x04); bus_dat(0x63);   /* ACVCOM */
    bus_cmd(0x01);                    /* Driver Output：300 gate (0x12B) */
    bus_dat((uint8_t)((g_panel_e042a13.panel_h - 1) & 0xFF));
    bus_dat((uint8_t)((g_panel_e042a13.panel_h - 1) >> 8));
    bus_dat(0x00);                    /* 扫描方向 B[2:0]=0 */
    bus_cmd(0x3A); bus_dat(0x2C);     /* dummy line period = 0x2C */
    bus_cmd(0x3B); bus_dat(0x0A);     /* gate line width = 0x0A */
    bus_cmd(0x3C); bus_dat(0x05);     /* BorderWaveform */
    bus_cmd(0x18); bus_dat(0x80);     /* 内置温度传感器自动模式 */
    bus_cmd(0x11); bus_dat(0x03);     /* 数据入口 X+ Y+ */

    /* RAM 窗口：X 0..width/8-1，Y 0..height-1（400x300 → 0x2F / 0x12B） */
    bus_cmd(0x44);
    bus_dat(0x00);
    bus_dat((uint8_t)(g_panel_e042a13.panel_w / 8 - 1));
    bus_cmd(0x45);
    bus_dat(0x00); bus_dat(0x00);
    bus_dat((uint8_t)((g_panel_e042a13.panel_h - 1) & 0xFF));
    bus_dat((uint8_t)((g_panel_e042a13.panel_h - 1) >> 8));

    bus_cmd(0x4E); bus_dat(0x00);     /* X 计数器归零 */
    bus_cmd(0x4F); bus_dat(0x00); bus_dat(0x00);   /* Y 计数器归零 */
    bus_wait_idle(&g_panel_e042a13, 5000);

    s_ready = true;
    return 0;
}

/* 双平面写入 + 真全刷（Waveshare Display_new 一比一）：
 * planes[0] = B/W 平面（bit=1 白）→ 0x24；
 * planes[1] = 红位平面（bit=1 红）→ 0x26（直通，见文件头语义注）；
 * 刷新走 0x22/0xF7 序列 + 0x20 Master Activation（SSD16xx 标准全刷，
 * 含上电/温度/LUT/显示，非 UC8176 的 0x12 直接激活） */
static int do_refresh(const uint8_t *bw_plane, const uint8_t *red_plane)
{
    LOG_I("full refresh begin");  /* 全刷路径打点：定位并发源
                                    * （A4 轮实锤 boot 期双路刷屏） */
    if (!s_ready && panel_init() != 0) return -1;

    const size_t plane_bytes =
        (size_t)(g_panel_e042a13.panel_w / 8) * g_panel_e042a13.panel_h;

    /* 地址计数器归零（无状态保证：不依赖上次写满后的回卷状态） */
    bus_cmd(0x4E); bus_dat(0x00);
    bus_cmd(0x4F); bus_dat(0x00); bus_dat(0x00);

    bus_cmd(0x24);
    bus_dat_stream(bw_plane, plane_bytes);
    bus_cmd(0x26);
    bus_dat_stream(red_plane, plane_bytes);

    bus_cmd(0x22); bus_dat(0xF7);     /* 全刷序列：上电+温度+LUT+显示 */
    bus_cmd(0x20);                    /* Master Activation */
    bus_wait_busy(&g_panel_e042a13, g_panel_e042a13.busy_timeout_ms);              /* 三色波形 ~16s：等真实完成再下电 */
    return 0;
}

static int panel_full_refresh(const uint8_t *frame)
{
    /* frame：双平面连续布局（Phase 2 约定）：plane_count x panel_w/8
     * x panel_h 字节，[0]=B/W 白位平面、[1]=红位平面 */
    const size_t plane_bytes =
        (size_t)(g_panel_e042a13.panel_w / 8) * g_panel_e042a13.panel_h;
    return do_refresh(frame, frame + plane_bytes);
}

static int panel_write_full(const uint8_t *frame)
{
    /* 竖屏原始帧直通全刷（epd_full_refresh / LAN 语义）：frame=NULL
     * 清白（Waveshare Clear_new：黑 RAM 全 0xFF + 红 RAM 全 0x00）。
     * 注：本路径不维护 prev 帧一致性，调用方须强制下一次全刷
     * （ui_force_full_refresh_next 已保证） */
    if (!frame) {
        if (!s_ready && panel_init() != 0) return -1;
        static const uint8_t white = 0xFF, no_red = 0x00;
        const size_t plane_bytes =
            (size_t)(g_panel_e042a13.panel_w / 8) * g_panel_e042a13.panel_h;
        bus_cmd(0x4E); bus_dat(0x00);
        bus_cmd(0x4F); bus_dat(0x00); bus_dat(0x00);
        bus_cmd(0x24);
        for (size_t i = 0; i < plane_bytes; i++) bus_dat(white);
        bus_cmd(0x26);
        for (size_t i = 0; i < plane_bytes; i++) bus_dat(no_red);
        bus_cmd(0x22); bus_dat(0xF7);
        bus_cmd(0x20);
        bus_wait_busy(&g_panel_e042a13, g_panel_e042a13.busy_timeout_ms);
        return 0;
    }
    return panel_full_refresh(frame);
}

static int panel_write_planes(const uint8_t *const *planes)
{
    /* 多平面统一入口（§9.3）：planes[] 指针数组（plane_count 项） */
    return do_refresh(planes[0], planes[1]);
}

/* —— BW-only 差分局刷（2026-09-01）：黑白兄弟屏 panel_e042a13bw.cpp
 * v8 定稿一比一移植，仅两处适配：
 *   ① prev/new 为双平面连续帧（plane_count=2），取首平面（B/W）
 *     作差分数据——红平面（偏移 plane_bytes 处）在局刷下不可表达；
 *   ② 传输保持 4MHz transfer 逐字节（本文件 bring-up 铁律；未随
 *     BW 版升 8MHz/DMA——隔离性原则，本次只加局刷不翻传输旧账）。
 * LUT 值/通道映射/两段式结构/0x22-0xEC 序列零改动（真机定稿勿盲调，
 * v1-v8 迭代史详见兄弟屏 k_lut_white/k_lut_dark 注释：单相残影叠加
 * →两段式先白后画→能量不对称锁死→跳 init 提速→去温度位→v7 单激活
 * 多相证伪（Rev 0.10 预发布 spec 与实测不符）→v8 回退 v6 定稿）。 */

/* 局刷回退为全刷（2026-09-02 终局结论）：
 * A1-A10h 共 16 轮系统性尝试（Mode 1 LUT / Mode 2 LUT / 0x37 Display
 * Option / 0x22 各种位组合），BUSY 始终 10-14s（= 全刷时间），自定义
 * LUT 从未被消费。交叉验证：GxEPD2 作者明确标注 4.2" 3-color 不支持
 * partial update；Arduino Forum "B/W/R 4.2" doesn't support partial
 * refresh"；仁波切 SSD1677 Mode 2 方案针对 ESL 价签屏（OTP/膜组不同）。
 * 结论：Hink E042A13-A0 三色屏硬件不支持局刷，partial ops 退化为全刷 */
static int panel_partial(const uint8_t *prev, const uint8_t *new_, uint8_t passes)
{
    (void)prev; (void)passes;
    LOG_I("partial: fallback to full refresh (3C HW no partial)");
    return do_refresh(new_, new_ + (size_t)(g_panel_e042a13.panel_w / 8) * g_panel_e042a13.panel_h);
}

static void panel_power_off(void)
{
    /* P2d：SSD16xx 族关电收敛 epd_bus（三家面板 byte 级一致） */
    bus_ssd16_power_off(&g_panel_e042a13, &s_ready);
}

static void panel_deep_sleep(void)
{
    bus_ssd16_deep_sleep(&s_ready);
}

/* —— desc 注册（第二面板单元，色彩面板首例）——
 * 时序：真机实测口径（2026-08-22 bring-up 收尾：全刷 14580ms 稳定
 * 复现）；BUSY 极性 HIGH=忙（SSD16xx 系，ReadBusy while(busy==1)
 * 实证）；plane 语义 0x24 bit=1 白 / 0x26 bit=1 红（四象限真机验证） */
const epd_panel_desc_t g_panel_e042a13 = {
    .name       = "e042a13_ssd1619",
    .controller = EPD_CTRL_SSD1619,
    .panel_w    = 400,
    .panel_h    = 300,
    .gfx_rotation = 0,       /* 横向原生面板，rotation=0 零转置直通
                              * （gfx 400x300，§6.3）；转置泛化已就绪 */
    .color_mode = EPD_COLOR_3C,
    .plane_count = 2,        /* B/W 白位平面 + 红位平面（§9.3） */
    .palette    = {
        /* 逻辑色 → 平面置位掩码：bit0=B/W 平面白位，bit1=红平面红位 */
        [EPD_GFX_WHITE]  = 0x01,   /* (bw=1,red=0) → 白 */
        [EPD_GFX_BLACK]  = 0x00,   /* (0,0) → 黑 */
        [EPD_GFX_ACCENT] = 0x02,   /* (0,1) → 红（真强调色，首次可用） */
        [EPD_GFX_AUX]    = 0x00,   /* 3C 无第四色 → 退化黑（§9.2） */
    },
    .accent_rgb = 0xFF0000,    /* 红（LAN 量化调色板注入） */
    .fb_location = EPD_FB_AUTO,   /* 双帧+画布 90KB < 128KB → 全 SRAM
                                   * （§10.2；与 WiFi/BLE 共存余量足） */
    .rst_pulse_ms = 20,
    .busy_level = 1,              /* BUSY=HIGH 忙（SSD16xx 系，与
                                   * UC8253 LOW=忙相反） */
    .busy_timeout_ms = 35000,     /* 实测忙 14580ms + 余量；2026-09-01
                                   * A4 轮 20s→35s：段2 40 帧 LUT 若按慢
                                   * 帧率（~0.7s/帧）需 ~28s，20s 必超时
                                   * ——先放开上限让波形跑完量真实时长 */
    .power_on_ms  = 40,           /* 上电含于 0x22/0xF7 序列，近似值 */
    .power_off_ms = 30,
    .full_ms    = 15000,          /* 真机实测 14580ms（2026-08-22
                                   * boot10/11 四次全刷一致）+余量 */
    .partial_ms = 15000,          /* 局刷退化为全刷（3C 硬件不支持局刷），
                                   * 与 full_ms 相同 */
    .partial_enabled = true,      /* 退化为全刷（见 panel_partial 注释）：
                                   * 上层调度仍走 partial 路径，实际执行全刷。
                                   * 保持 enabled 以兼容调度逻辑，但无速度收益 */
    .passes     = 1,              /* 两段式内含归白+出黑双驱动，单次够 */
    .partial_count_full_refresh = 16, /* 两段式每轮主动清残影（BW 兄弟屏
                                   * v6 先例 8→16）；三色全刷 14.6s
                                   * 打断重，保养低频为佳 */
    .window_8align = true,        /* 0x44 窗口 x 8 像素对齐（全屏 400 ✓） */
    .ops = {
        .init         = panel_init,
        .full_refresh = panel_full_refresh,
        .write_full   = panel_write_full,
        .partial      = panel_partial, /* BW-only 两段式差分局刷（v8 定稿） */
        .power_off    = panel_power_off,
        .deep_sleep   = panel_deep_sleep,
        .probe        = NULL,     /* SSD1619 版本读 0x2F→0x01（Waveshare
                                   * 实证）可作 probe，留 §14.3 接入 */
        .write_planes = panel_write_planes,
        .diag         = bus_diag_ssd16, /* T1.8：SSD16xx 0x2F 双读 */
    },
};
