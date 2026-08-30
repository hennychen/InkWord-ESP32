/**
 * @file panel_e042a13bw.cpp
 * @brief 4.2" 400x300 BW（HINK-E042A13-A0 黑白版，24Pin）面板单元（L0）
 *        —— SSD1619 手写序列 + BW desc
 *
 * 面板：HINK-E042A13-A0 黑白兄弟屏（标签编码 E042A22N183A68，N=无彩色层），
 * 4.2" 400x300 BW，24P FPC。驱动 IC SSD1619（与三色兄弟屏
 * panel_e042a13_ssd1619 同族，2026-08-30 bring-up 基于三色版实证序列）。
 *
 * 序列来源：Waveshare 4.2inch e-Paper (B) V2 官方 demo（同为 SSD1619
 * 400x300 BW）+ 三色兄弟屏 panel_e042a13_ssd1619.cpp 实证序列。
 * 与三色版关键差异：
 *   ① 仅 B/W 单平面（0x24），无红平面（0x26）写入；
 *   ② Display Update Control 0x22/0xC7（BW 路径，三色用 0xF7）；
 *   ③ 新增 0x21 Load Temperature for Update（BW demo 特有，三色版无此步）；
 *   ④ Init 核心序列与三色版相同（0x74/0x7E/0x0C/0x2B/0x01/0x3A/0x3B
 *      等，SSD1619 家族共用）。
 *
 * SPI 铁律（三色兄弟屏 bring-up 实证）：必须逐字节 SPI.transfer 连发，
 * writeBytes 不落地（ESP32-S3 Arduino core 事务兼容性问题）。
 *
 * bring-up 完成（2026-08-30 真机，实验 A）：0x22 参数 0xC7（Waveshare
 * BW demo 同款）实测 BUSY 仅 230ms 空转无波形（该 0xC7 适用 2020+
 * 批次 OTP）；换三色兄弟屏实证 0xF7 后波形真实运行，全刷实测
 * 2900/2901/2891ms（boot 三次稳定复现）。时序已按实测回填。
 * 局刷（2026-08-30 定稿，窗口方案）：RAM 窗口局刷 prev/new diff
 * 脏区包围盒 + 0x44/0x45 驱动窗口 + 0x22/0xFF（Waveshare epd4in2_V2
 * Display_Partial 同款屏官方序列），窗口外物理不扫描不闪烁。
 * 局刷前置完整重配（panel_init 无条件调）：本屏 2017 批次 Reset
 * 后须重发初始代码（0x0C/0x2B/0x74/0x7E 缺失升压跑偏），与 demo
 * （2020+ 批次 Reset 后 OTP 默认即可）不同。中间方案 0xFC 整屏
 * 差分（GxEPD2 GDEY042T81 序列）已证伪：只翻变化像素但全屏扫描
 * 可感知整屏闪烁，BUSY 2810ms 与全刷相当，弃用。
 * 遗留观察项（不阻塞）：预检 RST 脉冲无 BUSY 响应（SWRESET 0x12
 * 通路正常，全刷不依赖硬 RST）；0x2F 读回 0x21（三色版 0x01，
 * 型号/批次差异待研究）。
 */
#include "../epd_panel.h"
#include "../gpio_config.h"

#include <Arduino.h>
#include <SPI.h>
#include <string.h>    /* memcmp：局刷 diff 脏区检测 */

/* 前置声明：ops 实现引用 desc 几何/时序字段 */
extern const epd_panel_desc_t g_panel_e042a13bw;

static bool s_ready = false;   /* init 完成（power_off/deep_sleep 归零） */

/* SPI 时钟：2026-08-30 由 4MHz 提至 8MHz（RAM 传输 4 遍× 15000B
 * 占局刷总时长 ~1/3；本屏为 FPC 排线成品连接非杜邦线，倍频风险
 * 低——若出现花屏/乱码回退 4000000） */
static const uint32_t k_spi_hz = 8000000;

/* —— SPI 底层（与三色兄弟屏同源：逐字节 transfer，铁律不可换 writeBytes） —— */
static void epd_cmd(uint8_t c)
{
    SPI.beginTransaction(SPISettings(k_spi_hz, MSBFIRST, SPI_MODE0));
    digitalWrite(EPD_CS_PIN, LOW);
    digitalWrite(EPD_DC_PIN, LOW);    /* DC=0 命令 */
    SPI.transfer(c);
    digitalWrite(EPD_CS_PIN, HIGH);
    SPI.endTransaction();
}

static void epd_dat(uint8_t d)
{
    SPI.beginTransaction(SPISettings(k_spi_hz, MSBFIRST, SPI_MODE0));
    digitalWrite(EPD_CS_PIN, LOW);
    digitalWrite(EPD_DC_PIN, HIGH);   /* DC=1 数据 */
    SPI.transfer(d);
    digitalWrite(EPD_CS_PIN, HIGH);
    SPI.endTransaction();
}

/* 批量写 RAM：SPI.writeBytes DMA 块传输（2026-08-30 实证定稿：
 * 8MHz + FPC 排线 + 当前 core 显示正常——三色版“不落地”铁律
 * 局限于其 1MHz 杜邦线/旧 core 场景；DMA 后 RAM 传输 240→
 * ~80ms，局刷总时长 ~1.3s。若未来花屏回退 transfer 逐字节） */
#define RAM_WRITE_DMA 1
static void epd_write_buf(const uint8_t *p, size_t n)
{
    SPI.beginTransaction(SPISettings(k_spi_hz, MSBFIRST, SPI_MODE0));
    digitalWrite(EPD_CS_PIN, LOW);
    digitalWrite(EPD_DC_PIN, HIGH);
#if RAM_WRITE_DMA
    SPI.writeBytes(p, n);         /* DMA 块传输（TX-only 快路径） */
#else
    for (size_t i = 0; i < n; i++) SPI.transfer(p[i]);
#endif
    digitalWrite(EPD_CS_PIN, HIGH);
    SPI.endTransaction();
}

/* 等 BUSY 回空闲（单段，init/关电路径用；SSD16xx HIGH=忙） */
static void panel_wait_idle(uint32_t timeout_ms)
{
    const uint32_t t0 = millis();
    while (digitalRead(EPD_BUSY_PIN) == g_panel_e042a13bw.busy_level &&
           millis() - t0 < timeout_ms)
        delay(10);
}

/* 全屏 RAM 窗口 + 光标归零（写整屏帧前必调）：窗口局刷会残留小
 * 0x44/0x45 窗口，地址计数器被窗口约束行末回卷——若不重设，整屏
 * 帧连续写入会循环覆盖在小窗口内，其余 RAM 保持复位默认，全刷
 * 显示黑点花屏（2026-08-30 窗口局刷实验真机实证） */
static void set_full_window(void)
{
    epd_cmd(0x44);
    epd_dat(0x00);
    epd_dat((uint8_t)(g_panel_e042a13bw.panel_w / 8 - 1));
    epd_cmd(0x45);
    epd_dat(0x00); epd_dat(0x00);
    epd_dat((uint8_t)((g_panel_e042a13bw.panel_h - 1) & 0xFF));
    epd_dat((uint8_t)((g_panel_e042a13bw.panel_h - 1) >> 8));
    epd_cmd(0x4E); epd_dat(0x00);
    epd_cmd(0x4F); epd_dat(0x00); epd_dat(0x00);
}

/* 0x20 刷新序列后 BUSY 两段式等待（与三色兄弟屏同结构）：
 *   ① 等 BUSY 进入忙电平（≤300ms，容忍命令置位延迟）；
 *   ② 等 BUSY 释放（≤busy_timeout_ms，BW 波形应比三色 16s 快）。
 * profile 兼作「命令是否达 COG」现场判据 */
static void wait_refresh_done(void)
{
    const uint32_t t0 = millis();
    while (digitalRead(EPD_BUSY_PIN) != g_panel_e042a13bw.busy_level &&
           millis() - t0 < 300)
        delay(2);
    const bool asserted =
        digitalRead(EPD_BUSY_PIN) == g_panel_e042a13bw.busy_level;
    const uint32_t t1 = millis();
    while (digitalRead(EPD_BUSY_PIN) == g_panel_e042a13bw.busy_level &&
           millis() - t1 < g_panel_e042a13bw.busy_timeout_ms)
        delay(10);
    Serial.printf("[E042BW-DIAG] 0x20 busy: %s @%ums, active %ums\n",
                  asserted ? "HIGH" : "never",
                  (unsigned)(millis() - t0), (unsigned)(millis() - t1));
}

static int panel_init(void)
{
    /* SSD1619 初始化序列：与三色兄弟屏同源（Waveshare 4in2b_V2 Init_new +
     * GDEH042Z96 §4.1 初始代码），BW 版额外 0x21 Load Temperature for Update
     * （Waveshare BW demo 特有，为 BW 波形加载正确温度系数） */
    pinMode(EPD_RESET_PIN, OUTPUT);
    pinMode(EPD_CS_PIN, OUTPUT);
    pinMode(EPD_DC_PIN, OUTPUT);
    pinMode(EPD_BUSY_PIN, INPUT);
    digitalWrite(EPD_CS_PIN, HIGH);
    digitalWrite(EPD_DC_PIN, LOW);

    digitalWrite(EPD_RESET_PIN, HIGH);
    delay(10);
    digitalWrite(EPD_RESET_PIN, LOW);
    delay(g_panel_e042a13bw.rst_pulse_ms);
    digitalWrite(EPD_RESET_PIN, HIGH);
    delay(10);
    panel_wait_idle(5000);            /* 复位后 boot 自检（忙→闲） */

    epd_cmd(0x12);                    /* SWRESET：寄存器回 POR，VCOM 载 OTP */
    panel_wait_idle(5000);

    /* —— 官方初始代码（与三色版相同核心，SSD1619 家族共用）—— */
    epd_cmd(0x74); epd_dat(0x54);     /* Set Analog Block Control */
    epd_cmd(0x7E); epd_dat(0x3B);     /* Set Digital Block Control */
    epd_cmd(0x0C);                    /* Softstart：四段软启动 */
    epd_dat(0x8E); epd_dat(0x8C); epd_dat(0x85); epd_dat(0x3F);
    epd_cmd(0x2B); epd_dat(0x04); epd_dat(0x63);   /* ACVCOM */
    epd_cmd(0x01);                    /* Driver Output：300 gate (0x12B) */
    epd_dat((uint8_t)((g_panel_e042a13bw.panel_h - 1) & 0xFF));
    epd_dat((uint8_t)((g_panel_e042a13bw.panel_h - 1) >> 8));
    epd_dat(0x00);                    /* 扫描方向 B[2:0]=0 */
    epd_cmd(0x3A); epd_dat(0x2C);     /* dummy line period = 0x2C */
    epd_cmd(0x3B); epd_dat(0x0A);     /* gate line width = 0x0A */
    epd_cmd(0x3C); epd_dat(0x05);     /* BorderWaveform */
    epd_cmd(0x18); epd_dat(0x80);     /* 内置温度传感器自动模式 */
    epd_cmd(0x21); epd_dat(0x00);     /* Display Update Control（BW 特有，
                                       * 三色版无此步；Waveshare BW demo
                                       * 0x21/0x00 为 BW 波形温度加载路径） */
    epd_cmd(0x11); epd_dat(0x03);     /* 数据入口 X+ Y+ */

    /* RAM 窗口：X 0..width/8-1，Y 0..height-1（400x300 → 0x31 / 0x12B） */
    epd_cmd(0x44);
    epd_dat(0x00);
    epd_dat((uint8_t)(g_panel_e042a13bw.panel_w / 8 - 1));
    epd_cmd(0x45);
    epd_dat(0x00); epd_dat(0x00);
    epd_dat((uint8_t)((g_panel_e042a13bw.panel_h - 1) & 0xFF));
    epd_dat((uint8_t)((g_panel_e042a13bw.panel_h - 1) >> 8));

    epd_cmd(0x4E); epd_dat(0x00);     /* X 计数器归零 */
    epd_cmd(0x4F); epd_dat(0x00); epd_dat(0x00);   /* Y 计数器归零 */
    panel_wait_idle(5000);

    s_ready = true;
    return 0;
}

/* 单平面写入 + 全刷（BW 版：仅写 B/W 平面到 0x24，无红平面）：
 * 刷新走 0x22/0xC7 序列 + 0x20 Master Activation（BW 路径，
 * 三色版用 0xF7 含红平面驱动，BW 无需） */
static int do_refresh(const uint8_t *bw_plane)
{
    if (!s_ready && panel_init() != 0) return -1;
    const size_t plane_bytes =
        (size_t)(g_panel_e042a13bw.panel_w / 8) * g_panel_e042a13bw.panel_h;

    set_full_window();                /* 窗口显式重设：防局刷窗口残留 */

    epd_cmd(0x24);
    epd_write_buf(bw_plane, plane_bytes);

    epd_cmd(0x22); epd_dat(0xF7);     /* 全刷序列（实验 A 2026-08-30：0xC7 实测
                                       * BUSY 仅 230ms 空转无波形（Waveshare BW
                                       * demo 的 0xC7 适用 2020+ 批次 OTP）；
                                       * 换三色兄弟屏实证 0xF7（含平面驱动位
                                       * bit4/5，本屏 2017 批次同族 OTP） */
    epd_cmd(0x20);                    /* Master Activation */
    wait_refresh_done();              /* BW 波形应比三色 16s 快，实测回填 */
    return 0;
}

static int panel_full_refresh(const uint8_t *frame)
{
    /* frame：单平面帧（BW plane_count=1）：panel_w/8 * panel_h 字节 */
    return do_refresh(frame);
}

static int panel_write_full(const uint8_t *frame)
{
    /* 竖屏原始帧直通全刷：frame=NULL 清白（BW 版：B/W RAM 全 0xFF = 白屏，
     * 无红平面需清）。注：本路径不维护 prev 帧一致性，调用方须强制
     * 下一次全刷（ui_force_full_refresh_next 已保证） */
    if (!frame) {
        if (!s_ready && panel_init() != 0) return -1;
        static const uint8_t white = 0xFF;
        const size_t plane_bytes =
            (size_t)(g_panel_e042a13bw.panel_w / 8) * g_panel_e042a13bw.panel_h;
        set_full_window();            /* 窗口显式重设：防局刷窗口残留 */
        epd_cmd(0x24);
        for (size_t i = 0; i < plane_bytes; i++) epd_dat(white);
        epd_cmd(0x22); epd_dat(0xF7); /* 同 do_refresh：实验 A 0xF7 */
        epd_cmd(0x20);
        wait_refresh_done();
        return 0;
    }
    return panel_full_refresh(frame);
}

static int panel_write_planes(const uint8_t *const *planes)
{
    /* 多平面统一入口（§9.3）：BW 仅 planes[0]（单平面） */
    return do_refresh(planes[0]);
}

/* 快速差分局刷 LUT（SSD1619 0x32 寄存器波形，spec 图 6-6 格式）：
 * VS 区 35B：byte(L*7+n) 的 4×2bit = phase n 通道 A-D 在级 L 的电压
 * （00=VSS 01=VSH1 10=VSL 11=VSH2）；级 L0=B L1=W L2=R L3=R2
 * L4=VCOM；差分模式（0x26 old + 0x24 new）下像素对 (old,new)
 * 四组合分别走 L0(黑保持)/L1(白保持)/L2(黑→白)/L3(白→黑)。
 * TP/RP 区 35B：每组 TP[nA..nD]+RP[n]，TP=0 跳相、RP=0 跑 1 次。
 *
 * 波形迭代史（2026-08-30）：v1 单相 16 帧单向 480ms——局部刷新
 * 成功但残影叠加；v2 交替推挤——残影仍明显。根因不是翻转能量，
 * 而是保持通道零驱动使历史中间灰度残留永不修正、逐次累积。
 * v3 两段式“先白后画”（靠差分语义免白帧缓冲）：
 *   段1 (prev→全白)：旧内容黑像素走 L2 推白，历史残影一并
 *       清零；非变化区像素 (白,白) 走 L1 不动；
 *   段2 (全白→new)：新内容黑字从纯白态走 L3 单向推出，到位
 *       率最高；白像素保持不动
 * v4（残影终局）：v3 实测切换瞬间清晰、稳定后残影——粒子仅
 * 视觉到位未锁死，撤场后弛豫回弹，确系驱动能量不足。两段不
 * 对称加能：归白短（白底回弹不可见），出黑长（黑字锁死）
 * v5（提速）：实测段1=480/段2=959/init~500ms。①局刷跳过
 * panel_init（s_ready 直刷）：0xEC 无关闭位模拟保持使能，序列
 * 全量重写自持；全刷 0xF7 实证自带 OTP 波形重载（2900ms 正常，
 * 0x32 污染被自动洗掉）；power_off/deep_sleep 置 false 强制重配
 * ②段1 缩到 10 帧（白底到位率要求低）
 * v6（再提速）：0xFC→0xEC 去 I2C 温度读取位（寄存器 LUT 无
 * OTP 温度补偿需求，省 ~60ms×2）+ 归白 10→8 帧 + 保养阈值 8→16
 * v7（定稿）：单激活两相取代两段式——组 0 内 A 相推白 10 帧
 * + B 相出黑 40 帧，一次激活完成“先白后画”（实测 850ms@24 帧
 * 文字叠加：B 相 60% 能量不够黑；40 帧=两段式临界能量）。对比
 * 两段式：RAM 只写 2 遍（省 2 遍）+ 单次激活开销，总 ~1.16s vs
 * 1.29s；B 相 24 帧灰字实证淘汰，VCOM 摆动方案会扰动保持像素
 * v7（已回退）：单激活两相（LUT 组 0 内 A 相推白 + B 相出黑）
 * 四轮实验全部文字叠加：直刷 16 帧/A10+B24（850ms 结构精确
 * 生效）/A10+B40（1180ms 同）——旧字像素实际零驱动，而同值
 * 编码（0x55/0xAA 全字节）的 v1-v6 两段式推白正常。结论：
 * Rev 0.10 预发布 spec 图 6-6 的多相描述与芯片实测行为不符
 * （字节内 D7D6 分相电压未生效），待逻辑分析仪深挖，不再盲试。
 * v8 = 回退 v6 两段式定稿（实证最优） */
static const uint8_t k_lut_white[70] = {
    /* L0 B 保持：VSS */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* L1 W 保持：VSS */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* L2 黑→白：VSH1（段1 归白主驱动，全字节同值=位序无关） */
    0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
    /* L3 白→黑：VSL（段1 不触发，占位） */
    0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
    /* L4 VCOM：VSS */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* G0：A 相 8 帧（归白短波形，白底到位率要求最低） */
    0x08, 0x00, 0x00, 0x00, 0x00,
    /* G1..G6 全跳过 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* 出黑 LUT：A 相 40 帧——黑字粒子推到稳定端点锁死，撤场后
 * 不回弹（v3 回弹残影实证 16 帧不够） */
static const uint8_t k_lut_dark[70] = {
    /* L0 B 保持：VSS */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* L1 W 保持：VSS */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* L2 黑→白：VSH1（段2 不触发，占位） */
    0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
    /* L3 白→黑：VSL（段2 出黑主驱动，从纯白态出发） */
    0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
    /* L4 VCOM：VSS */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* G0：A 相 40 帧（出黑长波形，锁死黑字粒子） */
    0x28, 0x00, 0x00, 0x00, 0x00,
    /* G1..G6 全跳过 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static int panel_partial(const uint8_t *prev, const uint8_t *new_, uint8_t passes)
{
    const int wb = g_panel_e042a13bw.panel_w / 8;
    const int ph = g_panel_e042a13bw.panel_h;

    /* diff 判断：帧无变化免刷（spec 查证 0x44/0x45 仅 RAM 写窗口，
     * 对驱动范围无约束，窗口优化无意义——硬件整屏扫，靠 LUT 保持
     * 通道零驱动实现“未变区域不闪”） */
    int dirty = 0;
    for (int y = 0; y < ph && !dirty; y++)
        if (memcmp(prev + (size_t)y * wb, new_ + (size_t)y * wb, wb) != 0)
            dirty = 1;
    if (!dirty) return 0;

    /* 实验 D 2026-08-30（SSD1619A0 spec 查证定案）：本屏无硬件窗口
     * 局刷（无 0x90/0x91/0x92 命令；0x44/0x45 小窗口激活空转三轮
     * 实证），“partial update”唯一正解 = 0x26(old)+0x24(new) 差分
     * + 0x32 寄存器 LUT 快速波形 + 0x22/0xEC（bit4 Load LUT +
     * Mode2 差分，激活时加载刚写入的寄存器 LUT）。
     * 演进：v3-v6 两段式（两次激活两次差分基准）→ v7 单激活两相
     * （LUT 组 0 内 A/B 相序内嵌同样时序，一次激活，RAM 少写
     * 2 遍）——能量学等价：A 相=归白，B 相=从白态出黑 */
    if (!s_ready && panel_init() != 0) return -1;  /* v5：局刷链路状态
                                        * 自持（0xEC 不关模拟，RAM/LUT 全量
                                        * 重写），跳过硬复位开销 ~500ms；
                                        * 关电/深睡后 s_ready=false 自动
                                        * 重配 */

    /* v8 = v6 两段式回退定稿（单激活多相 spec 与实测不符，见上）：
     * 段1 (prev→全白) 归白清残影，段2 (全白→new) 出黑锁死 */
    epd_cmd(0x21); epd_dat(0x00); epd_dat(0x00);
    const size_t plane_bytes = (size_t)wb * ph;

    /* 段1：归白（短 LUT）。0x26=prev（当前内容），0x24=全 0xFF */
    epd_cmd(0x32);
    epd_write_buf(k_lut_white, sizeof(k_lut_white));
    set_full_window();
    epd_cmd(0x26);
    epd_write_buf(prev, plane_bytes);
    epd_cmd(0x24);
    for (size_t i = 0; i < plane_bytes; i++) epd_dat(0xFF);
    epd_cmd(0x22); epd_dat(0xEC);   /* CLK+Analog+LoadLUT+Mode2+
                                   * DISPLAY（去温度位，寄存器 LUT
                                   * 无需 OTP 温度补偿） */
    epd_cmd(0x20);
    wait_refresh_done();

    /* 段2：出新（长 LUT 锁死黑字，两段间重写 0x32——RAM 写命令
     * 不破坏 LUT 寄存器，激活时 0xEC bit4 重新加载） */
    for (uint8_t p = 0; p < passes; p++) {
        epd_cmd(0x32);
        epd_write_buf(k_lut_dark, sizeof(k_lut_dark));
        set_full_window();
        epd_cmd(0x26);
        for (size_t i = 0; i < plane_bytes; i++) epd_dat(0xFF);
        epd_cmd(0x24);
        epd_write_buf(new_, plane_bytes);
        epd_cmd(0x22); epd_dat(0xEC);
        epd_cmd(0x20);
        wait_refresh_done();            /* 实测段共 ~1.29s */
    }
    return 0;
}

static void panel_power_off(void)
{
    /* SSD16xx 标准关电序列（与三色版相同）：0x22/0xC3 + 0x20。
     * 完成后归零 s_ready —— 下次刷新完整重配（无状态铁律） */
    if (!s_ready) return;
    epd_cmd(0x22); epd_dat(0xC3);
    epd_cmd(0x20);
    panel_wait_idle(g_panel_e042a13bw.busy_timeout_ms);
    s_ready = false;
}

static void panel_deep_sleep(void)
{
    /* Waveshare Sleep 一比一：0x10 check 0x01 深睡（~µA 级），
     * RST 硬复位唤醒 + panel_init 重初始化 */
    epd_cmd(0x10); epd_dat(0x01);
    s_ready = false;
}

/* —— desc 注册（BW 面板，与三色兄弟屏同族 SSD1619）——
 * 时序实测口径（2026-08-30 实验 A）：0x22/0xF7 全刷 2.9s 稳定复现；
 * BUSY 极性 HIGH=忙（SSD16xx 系，与三色版一致） */
const epd_panel_desc_t g_panel_e042a13bw = {
    .name       = "e042a13bw_ssd1619",
    .controller = EPD_CTRL_SSD1619,
    .panel_w    = 400,
    .panel_h    = 300,
    .gfx_rotation = 0,       /* 横向原生面板（gfx 400x300，LAYOUT_MID） */
    .color_mode = EPD_COLOR_BW,
    .plane_count = 1,        /* BW 单平面（三色兄弟为 2） */
    .palette    = {
        /* BW 语义同 panel_depg0370（§9.3/§9.4：bit0=B/W 平面白位） */
        [EPD_GFX_WHITE]  = 0x01,
        [EPD_GFX_BLACK]  = 0x00,
        [EPD_GFX_ACCENT] = 0x00,   /* BW 退化：强调色降级黑（§9.2） */
        [EPD_GFX_AUX]    = 0x00,
    },
    .accent_rgb = 0x000000,    /* BW 无第三色，LAN 调色板不注入 */
    .fb_location = EPD_FB_AUTO,    /* 单帧+画布 ~15KB 全 SRAM（三色兄弟 45KB） */
    .rst_pulse_ms = 20,
    .busy_level = 1,               /* SSD16xx 系 HIGH 忙（兄弟屏实证） */
    .busy_timeout_ms = 6000,      /* 实测忙 2900ms（2026-08-30 boot 三次
                                   * 全刷 2900/2901/2891ms）+ 2 倍余量 */
    .power_on_ms  = 40,
    .power_off_ms = 30,
    .full_ms    = 3000,           /* 真机实测 ~2.9s（0x22/0xF7 波形，
                                   * 2026-08-30 实验 A）+ 余量 */
    .partial_ms = 1300,           /* 两段式 + DMA：波形 320+970ms
                                   * + RAM 传输 ~80ms（2026-08-30
                                   * DMA 定稿） */
    .partial_enabled = true,      /* 0x32 两段式差分局刷（v8=v6
                                   * 回退定稿，详见 k_lut_white/
                                   * k_lut_dark 注释） */
    .passes     = 1,              /* 差分局刷单次波形；对比度实测不足
                                   * 再调 2（L3 可覆盖） */
    .partial_count_full_refresh = 16, /* v6：8→16——两段式每轮主动清
                                   * 残影，全刷保养频率可减半，降低
                                   * 2.9s 全刷打断体验 */
    .window_8align = true,         /* 0x44 窗口 x 8 像素对齐（全屏 400 ✓） */
    .ops = {
        .init         = panel_init,
        .full_refresh = panel_full_refresh,
        .write_full   = panel_write_full,
        .partial      = panel_partial,   /* 双 RAM 差分局刷（0xFC） */
        .power_off    = panel_power_off,
        .deep_sleep   = panel_deep_sleep,
        .probe        = NULL,       /* SSD1619 版本读 0x2F→0x01 可作 probe */
        .write_planes = panel_write_planes,
    },
};
