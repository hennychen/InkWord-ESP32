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
 * 快刷实验结论（2026-08-22，勿重试）：同族 SSD1683 的黑白快刷位
 * 组合 0x22/0xDC 在本屏 0x20 后 BUSY 仅 259ms 空转（无波形输出，
 * 物理上不可能完成翻转）→ SSD1619 OTP 无差分快刷波形路径，
 * 14.6s 全刷即本屏下限；提速仅余假高温选波（0x1A 写温度 + 0x91
 * 载 LUT，SSD1683 实测 15.1→10.8s）与自定义 LUT（0x32）两条路，
 * 均有残影/对比度风险。
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

#include <Arduino.h>
#include <SPI.h>

/* 前置声明：ops 实现引用 desc 几何/时序字段 */
extern const epd_panel_desc_t g_panel_e042a13;

static bool s_ready = false;   /* init 完成（power_off/deep_sleep 归零） */

/* —— SPI 底层（epd_driver_init 已 SPI.begin(7,-1,8,10)，此处事务直发） —— */
static void epd_cmd(uint8_t c)
{
    SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    digitalWrite(EPD_CS_PIN, LOW);
    digitalWrite(EPD_DC_PIN, LOW);    /* DC=0 命令 */
    SPI.transfer(c);
    digitalWrite(EPD_CS_PIN, HIGH);
    SPI.endTransaction();
}

static void epd_dat(uint8_t d)
{
    SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    digitalWrite(EPD_CS_PIN, LOW);
    digitalWrite(EPD_DC_PIN, HIGH);   /* DC=1 数据 */
    SPI.transfer(d);
    digitalWrite(EPD_CS_PIN, HIGH);
    SPI.endTransaction();
}

/* 批量写 RAM（bring-up 实证修正 2026-08-22）：SPI.writeBytes 路径
 * RAM 不生效（boot6/7/8 三轮不同初始化均恒定全红 = RAM 保持硬复位
 * 默认 red全1/bw全0，写入从未落地；换 transfer 逐字节连发后四象限
 * 立即显现，同变量对照实锤）。单事务内 transfer 连发与 epd_dat 逻辑
 * 同源，仅 CS/DC 开销一次；15KB @4MHz + 每字节软开销 <100ms。根因
 * 未深究（疑似 ESP32-S3 Arduino core writeBytes 事务兼容性问题），
 * 后续若需批量性能可查 SPI.writeBytes(p,n,true) 三参重载 */
static void epd_write_buf(const uint8_t *p, size_t n)
{
    SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    digitalWrite(EPD_CS_PIN, LOW);
    digitalWrite(EPD_DC_PIN, HIGH);
    for (size_t i = 0; i < n; i++) SPI.transfer(p[i]);
    digitalWrite(EPD_CS_PIN, HIGH);
    SPI.endTransaction();
}

/* 等 BUSY 回空闲（单段，init/关电路径用；SSD16xx HIGH=忙） */
static void panel_wait_idle(uint32_t timeout_ms)
{
    const uint32_t t0 = millis();
    while (digitalRead(EPD_BUSY_PIN) == g_panel_e042a13.busy_level &&
           millis() - t0 < timeout_ms)
        delay(10);
}

/* 0x20 刷新序列后 BUSY 两段式等待（诊断 + 修正，2026-08-22 bring-up）：
 *   ① 等 BUSY 进入忙电平（≤300ms，容忍命令置位延迟）；
 *   ② 等 BUSY 释放（≤busy_timeout_ms，三色波形 ~16s）。
 * profile 兼作「命令是否达 COG」现场判据：
 *   - busy 持续 ~16s：波形真实运行，刷屏成功；
 *   - busy 从未置位：SPI 命令未达 COG（硬件排查） */
static void wait_refresh_done(void)
{
    const uint32_t t0 = millis();
    while (digitalRead(EPD_BUSY_PIN) != g_panel_e042a13.busy_level &&
           millis() - t0 < 300)
        delay(2);
    const bool asserted =
        digitalRead(EPD_BUSY_PIN) == g_panel_e042a13.busy_level;
    const uint32_t t1 = millis();
    while (digitalRead(EPD_BUSY_PIN) == g_panel_e042a13.busy_level &&
           millis() - t1 < g_panel_e042a13.busy_timeout_ms)
        delay(10);
    Serial.printf("[E042-DIAG] 0x20 busy: %s @%ums, active %ums\n",
                  asserted ? "HIGH" : "never",
                  (unsigned)(millis() - t0), (unsigned)(millis() - t1));
}

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
    panel_wait_idle(5000);            /* 复位后 boot 自检（忙→闲） */

    epd_cmd(0x12);                    /* SWRESET：寄存器回 POR，VCOM 载 OTP */
    panel_wait_idle(5000);

    /* —— 官方初始代码（§4.1 步骤 3，参数取命令表标定值）—— */
    epd_cmd(0x74); epd_dat(0x54);     /* Set Analog Block Control */
    epd_cmd(0x7E); epd_dat(0x3B);     /* Set Digital Block Control */
    epd_cmd(0x0C);                    /* Softstart：四段软启动 */
    epd_dat(0x8E); epd_dat(0x8C); epd_dat(0x85); epd_dat(0x3F);
    epd_cmd(0x2B); epd_dat(0x04); epd_dat(0x63);   /* ACVCOM */
    epd_cmd(0x01);                    /* Driver Output：300 gate (0x12B) */
    epd_dat((uint8_t)((g_panel_e042a13.panel_h - 1) & 0xFF));
    epd_dat((uint8_t)((g_panel_e042a13.panel_h - 1) >> 8));
    epd_dat(0x00);                    /* 扫描方向 B[2:0]=0 */
    epd_cmd(0x3A); epd_dat(0x2C);     /* dummy line period = 0x2C */
    epd_cmd(0x3B); epd_dat(0x0A);     /* gate line width = 0x0A */
    epd_cmd(0x3C); epd_dat(0x05);     /* BorderWaveform */
    epd_cmd(0x18); epd_dat(0x80);     /* 内置温度传感器自动模式 */
    epd_cmd(0x11); epd_dat(0x03);     /* 数据入口 X+ Y+ */

    /* RAM 窗口：X 0..width/8-1，Y 0..height-1（400x300 → 0x2F / 0x12B） */
    epd_cmd(0x44);
    epd_dat(0x00);
    epd_dat((uint8_t)(g_panel_e042a13.panel_w / 8 - 1));
    epd_cmd(0x45);
    epd_dat(0x00); epd_dat(0x00);
    epd_dat((uint8_t)((g_panel_e042a13.panel_h - 1) & 0xFF));
    epd_dat((uint8_t)((g_panel_e042a13.panel_h - 1) >> 8));

    epd_cmd(0x4E); epd_dat(0x00);     /* X 计数器归零 */
    epd_cmd(0x4F); epd_dat(0x00); epd_dat(0x00);   /* Y 计数器归零 */
    panel_wait_idle(5000);

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
    if (!s_ready && panel_init() != 0) return -1;
    const size_t plane_bytes =
        (size_t)(g_panel_e042a13.panel_w / 8) * g_panel_e042a13.panel_h;

    /* 地址计数器归零（无状态保证：不依赖上次写满后的回卷状态） */
    epd_cmd(0x4E); epd_dat(0x00);
    epd_cmd(0x4F); epd_dat(0x00); epd_dat(0x00);

    epd_cmd(0x24);
    epd_write_buf(bw_plane, plane_bytes);
    epd_cmd(0x26);
    epd_write_buf(red_plane, plane_bytes);

    epd_cmd(0x22); epd_dat(0xF7);     /* 全刷序列：上电+温度+LUT+显示 */
    epd_cmd(0x20);                    /* Master Activation */
    wait_refresh_done();              /* 三色波形 ~16s：等真实完成再下电 */
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
        epd_cmd(0x4E); epd_dat(0x00);
        epd_cmd(0x4F); epd_dat(0x00); epd_dat(0x00);
        epd_cmd(0x24);
        for (size_t i = 0; i < plane_bytes; i++) epd_dat(white);
        epd_cmd(0x26);
        for (size_t i = 0; i < plane_bytes; i++) epd_dat(no_red);
        epd_cmd(0x22); epd_dat(0xF7);
        epd_cmd(0x20);
        wait_refresh_done();
        return 0;
    }
    return panel_full_refresh(frame);
}

static int panel_write_planes(const uint8_t *const *planes)
{
    /* 多平面统一入口（§9.3）：planes[] 指针数组（plane_count 项） */
    return do_refresh(planes[0], planes[1]);
}

static void panel_power_off(void)
{
    /* SSD16xx 标准关电序列（GxEPD2 GDEY042Z98/_PowerOff 同款）：
     * 0x22/0xC3 + 0x20。完成后归零 s_ready —— 下次刷新完整重配
     * （无状态铁律，不赌关电后 RAM 窗口/计数器存活） */
    if (!s_ready) return;
    epd_cmd(0x22); epd_dat(0xC3);
    epd_cmd(0x20);
    panel_wait_idle(g_panel_e042a13.busy_timeout_ms);
    s_ready = false;
}

static void panel_deep_sleep(void)
{
    /* Waveshare Sleep_new 一比一：0x10 check 0x01 深睡（~µA 级），
     * RST 硬复位唤醒 + panel_init 重初始化（demo 注释实证路径） */
    epd_cmd(0x10); epd_dat(0x01);
    s_ready = false;
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
    .fb_location = EPD_FB_AUTO,   /* 双帧+画布 90KB < 128KB → 全 SRAM
                                   * （§10.2；与 WiFi/BLE 共存余量足） */
    .rst_pulse_ms = 20,
    .busy_level = 1,              /* BUSY=HIGH 忙（SSD16xx 系，与
                                   * UC8253 LOW=忙相反） */
    .busy_timeout_ms = 20000,     /* 实测忙 14580ms + 余量 */
    .power_on_ms  = 40,           /* 上电含于 0x22/0xF7 序列，近似值 */
    .power_off_ms = 30,
    .full_ms    = 15000,          /* 真机实测 14580ms（2026-08-22
                                   * boot10/11 四次全刷一致）+余量 */
    .partial_ms = 16000,          /* 无快速局刷：partial==full */
    .partial_enabled = false,     /* 三色面板一律 false（§13.2 厂商级确认） */
    .passes     = 1,
    .partial_count_full_refresh = 1, /* 无局刷：阈值不参与调度，保守 1 */
    .window_8align = true,        /* 0x44 窗口 x 8 像素对齐（全屏 400 ✓） */
    .ops = {
        .init         = panel_init,
        .full_refresh = panel_full_refresh,
        .write_full   = panel_write_full,
        .partial      = NULL,     /* partial_enabled=false，L3 不触达 */
        .power_off    = panel_power_off,
        .deep_sleep   = panel_deep_sleep,
        .probe        = NULL,     /* SSD1619 版本读 0x2F→0x01（Waveshare
                                   * 实证）可作 probe，留 §14.3 接入 */
        .write_planes = panel_write_planes,
    },
};
