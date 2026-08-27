/**
 * @file panel_wft0290.cpp
 * @brief 2.9" 128x296 BW 面板单元 —— UC8151D 序列（GxEPD2_290_T5D
 *        忠实移植），bring-up 三轮收敛
 *
 * 屏体识别（2026-08-26 实物标签 + 运行时证据 + 外部对照）：
 *   屏体标签 WF0290T5PCZ10230H（维峰 WEIFENG），排线索号
 *   WFT0290CZ10LP（最初记录的「WFT0290CZ10」是排线不是屏体）。
 *   GxEPD2 官方选型表：GDEW029T5/T5D/M06/I6FD 128x296 均注
 *   (WFT0290CZ10)，控制器 UC8151 / UC8151D（IL0373 系）——
 *   本屏 = GDEW029T5D 同族。
 *
 * bring-up 轮次（.bak/.bak2/.bak3 留档）：
 *   一轮 SSD1680 假设证伪：BUSY 空闲 HIGH（SSD16xx 新代为 LOW）+
 *     0x2F 读浮空 0xFF（无 SSD 系状态命令）；
 *   二轮 UC8253 demo 移植（.bak3）：0x04/0x12/0x02 链真实应答
 *     （0x12 后 BUSY 忙 3370ms + 屏面闪烁——UC 家族命令同义），
 *     但 4736 字节 0xFF 刷出花屏雪花——根因：UC8151D 与 UC8253
 *     的 PSR/分辨率配置不兼容：
 *       ① PSR(0x00) UC8151D 只收 1 字节（0x1F），UC8253 demo 发
 *          2 字节（0xDF+0x0D），第二字节被误锁存；
 *       ② UC8253 demo 靠 OTP 默认分辨率（DEPG0370 恰好匹配），
 *          UC8151D 必须显式发 0x61 TRES 设 128x296——缺发时
 *          gate/source 数错乱 → RAM 数据与屏面映射错位 → 花屏。
 *     交叉铁证：GxEPD2_290_T5D 实测全刷 3251ms，本屏实测 3370ms
 *     同款量级（同 LUT 同波形钟控）。
 *   三轮（本版）：GxEPD2_290_T5D 序列一比一（.pio/libdeps 本地库），
 *     PSR 0x1F + TRES 0x80/0x01/0x28 + CDI 0x97。
 *
 * 序列（GxEPD2_290_T5D._InitDisplay/_Init_Full/_Update_Full 忠实）：
 *   init  = RST 脉冲 + PSR(0x00: 0x1F，OTP LUT 128x296) +
 *           TRES(0x61: 0x80 0x01 0x28 = source 128 / gate 296) +
 *           CDI(0x50: 0x97 全刷边框白)；
 *   刷新  = 0x04 上电(BUSY) → 0x13 整屏写 4736B → 0x12 刷新(BUSY
 *           ~3.4s) → 0x02 关电(BUSY)；0x12 后不发哑字节（GxEPD2
 *           路径；DEPG demo 的 0x00 哑字节系 UC8253 demo 风格）；
 *   深睡  = 0x07/0xA5（RST 硬复位唤醒）；BUSY=LOW 忙（UC 家族，
 *   空闲 HIGH，GxEPD2 构造 LOW busy_level + 二轮实测双证）。
 *
 * 局刷（2026-08-26 移植，GxEPD2_290_T5D._Init_Part + DEPG0370
 * Plan B 融合）：REG LUT 快刷（GxEPD2 实测 705ms）——PSR 0xBF +
 * 0x82/0x08 + CDI 0x17 + 0x20~0x24 五张 LUT；数据走无窗口整屏
 * 双 RAM 差分（DEPG0370 先例：UC 族 partial window 实证不可靠，
 * 整屏 0x10+0x13 让 COG 全屏差分，跳过不变像素）。
 *
 * 几何：128x296 竖屏 rotation=0（gfx 128x296，LAYOUT_TINY 档）；
 * PSR 0x1F 扫描向为 GxEPD2 默认（UD=1/SHL=1），四象限标定后修正。
 * 时序：GxEPD2_290_T5D 实测口径（全刷 3251/上电 97/下电 40ms），
 * 本屏白屏试刷复测回填。
 *
 * 无状态设计（desc 头注释铁律）：s_ready 前置检查，power_off/
 * deep_sleep 后归零，下次刷新 RST 唤醒 + 完整重配。
 */
#include "../epd_panel.h"
#include "../gpio_config.h"

#include <Arduino.h>
#include <SPI.h>

/* 前置声明：ops 实现引用 desc 几何/时序字段 */
extern const epd_panel_desc_t g_panel_wft0290;

static bool s_ready = false;   /* uc_init 完成（power_off/deep_sleep 归零） */

/* —— SPI 底层（panel_wf0270_ssd1680 同款实证路径：transfer 逐字节
 * 连发，SPI.writeBytes 在 ESP32-S3 Arduino core 有 RAM 不落地陷阱，
 * 禁用；epd_driver_init 已 SPI.begin，此处事务直发）—— */
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

static void epd_write_buf(const uint8_t *p, size_t n)
{
    SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    digitalWrite(EPD_CS_PIN, LOW);
    digitalWrite(EPD_DC_PIN, HIGH);
    for (size_t i = 0; i < n; i++) SPI.transfer(p[i]);
    digitalWrite(EPD_CS_PIN, HIGH);
    SPI.endTransaction();
}

/* 等 BUSY 回空闲（UC 家族 LOW=忙，回 HIGH 空闲） */
static void wait_idle_level(uint8_t busy_level, uint32_t timeout_ms)
{
    const uint32_t t0 = millis();
    while (digitalRead(EPD_BUSY_PIN) == busy_level &&
           millis() - t0 < timeout_ms)
        delay(10);
}

/* 0x12 刷新后 BUSY 两段式等待（诊断 + 修正，DEPG0370/WF0270 bring-up
 * 模式）：① 等 BUSY 进入忙电平（≤300ms 容忍命令置位延迟）；
 * ② 等释放（≤busy_timeout_ms）。busy 从未置位 = 0x12 未达 COG
 * （SPI 硬件排查判据）。忙时长偏离 ~3.4s = 波形/供电异常信号。 */
static void wait_refresh_done(void)
{
    const uint32_t t0 = millis();
    while (digitalRead(EPD_BUSY_PIN) != g_panel_wft0290.busy_level &&
           millis() - t0 < 300)
        delay(2);
    const bool asserted =
        digitalRead(EPD_BUSY_PIN) == g_panel_wft0290.busy_level;
    const uint32_t t_enter = millis() - t0;   /* 置忙延迟（打印前捕获，
                                                * 二轮版此处取值口径有误） */
    const uint32_t t1 = millis();
    while (digitalRead(EPD_BUSY_PIN) == g_panel_wft0290.busy_level &&
           millis() - t1 < g_panel_wft0290.busy_timeout_ms)
        delay(10);
    Serial.printf("[W29-DIAG] 0x12 busy: %s @%ums, active %ums\n",
                  asserted ? "LOW" : "never",
                  (unsigned)t_enter, (unsigned)(millis() - t1));
}

/* —— bring-up 通电自检（SOP 步 1，三轮收敛为换屏/重插 FPC 验证）——
 * 判据（UC8151D 实锤口径）：RST 脉冲后 BUSY 忙→闲往返 = COG 在位；
 * 空闲电平 HIGH = UC 家族。异常 → fail-safe -1。 */
static int detect_alive(void)
{
    pinMode(EPD_RESET_PIN, OUTPUT);
    pinMode(EPD_CS_PIN, OUTPUT);
    pinMode(EPD_DC_PIN, OUTPUT);
    pinMode(EPD_BUSY_PIN, INPUT);
    digitalWrite(EPD_CS_PIN, HIGH);
    digitalWrite(EPD_DC_PIN, LOW);

    /* RST：HIGH 200ms → LOW rst_pulse → HIGH（三轮实测应答良好口径） */
    digitalWrite(EPD_RESET_PIN, HIGH);
    delay(200);
    digitalWrite(EPD_RESET_PIN, LOW);
    delay(10);
    digitalWrite(EPD_RESET_PIN, HIGH);

    /* 证据①：RST 释放后 5s 窗口观察 BUSY 忙→闲往返（COG boot 自检） */
    const uint32_t t0 = millis();
    bool saw_high = false, saw_low = false;
    while (millis() - t0 < 5000 && !(saw_high && saw_low)) {
        if (digitalRead(EPD_BUSY_PIN)) saw_high = true;
        else saw_low = true;
        delay(10);
    }
    const bool cog_alive = saw_high && saw_low;

    /* 证据②：静置 300ms 后采样 5 次取众数（空闲 HIGH = UC 家族） */
    delay(300);
    int high_cnt = 0;
    for (int i = 0; i < 5; i++) {
        if (digitalRead(EPD_BUSY_PIN)) high_cnt++;
        delay(10);
    }

    Serial.printf("[W29-DIAG] RST pulse: H%s L%s (alive=%d) | "
                  "BUSY idle: %s (%d/5 HIGH)\n",
                  saw_high ? "+" : "-", saw_low ? "+" : "-",
                  cog_alive, high_cnt >= 3 ? "HIGH" : "LOW", high_cnt);

    if (!cog_alive) {
        Serial.printf("[W29-DIAG] COG no answer: check FPC seat / "
                      "VCI 3.3V / BS=LOW(4-line SPI) wiring\n");
        return -1;
    }
    if (high_cnt < 3) {
        Serial.printf("[W29-DIAG] idle LOW (SSD16xx traits) — NOT this "
                      "panel's family (UC8151D 实锤 idle HIGH); check "
                      "FPC seat / wiring\n");
        return -1;
    }
    return 0;
}

/* —— UC8151D init（GxEPD2_290_T5D._InitDisplay 忠实）——
 * demo 板 EN 脚（VCI 使能）为 DEPG demo 板专属，本驱动板 VCI 直供。 */
static int uc_init(void)
{
    digitalWrite(EPD_RESET_PIN, HIGH);
    delay(200);
    digitalWrite(EPD_RESET_PIN, LOW);
    delay(g_panel_wft0290.rst_pulse_ms);
    digitalWrite(EPD_RESET_PIN, HIGH);
    wait_idle_level(g_panel_wft0290.busy_level, 5000);  /* boot 自检 */

    /* PSR：单字节 0x1F = OTP LUT + 128x296 + 默认扫描（UC8253 的
     * 双字节 PSR 在 UC8151D 上第二字节会被误锁存，二轮花屏根因①） */
    epd_cmd(0x00);
    epd_dat(0x1F);

    /* TRES：分辨率显式设置 source=128 / gate=296（二轮花屏根因②，
     * UC8253 demo 靠 OTP 默认不发的命令，UC8151D 必发） */
    epd_cmd(0x61);
    epd_dat((uint8_t)g_panel_wft0290.panel_w);               /* 128 */
    epd_dat((uint8_t)(g_panel_wft0290.panel_h >> 8));        /* 296 高位 */
    epd_dat((uint8_t)(g_panel_wft0290.panel_h & 0xFF));      /* 296 低位 */

    /* CDI：VCOM 与数据间隔（全刷边框白，GxEPD2 原注释 WBmode:VBDF 17） */
    epd_cmd(0x50);
    epd_dat(0x97);

    s_ready = true;
    return 0;
}

/* B/W 整屏写 + 全刷（GxEPD2_290_T5D._Init_Full/_Update_Full 忠实）：
 * 0x04 上电(BUSY ~100ms) → 0x13 写 4736B（bit=1 白）→ 0x12 刷新
 * (BUSY ~3.4s) → 0x02 关电(BUSY ~40ms)。frame 约定同 DEPG0370：
 * 竖屏整帧 panel_w/8 x panel_h 字节。全刷 LUT 用新 RAM（0x13）驱动
 * （DEPG Epaper_Load_White 只写 0x13 同先例；局刷才需双 RAM）。 */
static int uc_refresh_bw(const uint8_t *frame /* NULL = 全白 */)
{
    const size_t plane_bytes =
        (size_t)(g_panel_wft0290.panel_w / 8) * g_panel_wft0290.panel_h;

    epd_cmd(0x04);                     /* Power on（GxEPD2 先上电再写 RAM） */
    wait_idle_level(g_panel_wft0290.busy_level,
                    g_panel_wft0290.busy_timeout_ms);

    epd_cmd(0x13);                     /* DTM1 新帧（地址计数器 RST 归零） */
    if (frame) {
        epd_write_buf(frame, plane_bytes);
    } else {
        for (size_t i = 0; i < plane_bytes; i++) epd_dat(0xFF);  /* 白 */
    }

    epd_cmd(0x12);                     /* display refresh（无哑字节） */
    wait_refresh_done();

    epd_cmd(0x02);                     /* Power off */
    wait_idle_level(g_panel_wft0290.busy_level, 1000);
    return 0;
}

/* —— REG LUT 局刷（GxEPD2_290_T5D._Init_Part 移植）——
 * 五张 LUT（GxEPD2_290_T5D.cpp L361-416）：0x20 VCOM/DC、0x21 WW、
 * 0x22 BW、0x23 WB、0x24 BB。快刷波形只驱动变化像素，本屏实测
 * 709ms/次（GxEPD2 同款 705ms）vs 全刷 3370ms。
 * 相长 Tx19 调优史：双刷（passes=2）时代 0x20 淡影 → 0x28 无改善；
 * 实锤残影/回退主因为第二遍同向过驱动（单相 LUT 粒子过冲回弹）
 * → passes=1 后回退消失、残影明显改善；单刷 0x20（710ms 实测）
 * 复测淡影回归 + 文字变浅（白→黑翻转同样不足）→ 终值 0x28
 * （869ms，黑度正常 + 残影明显改善的甜点值）。
 *
 * 四相推拉实验证伪留档（2026-08-26）：M06 同族揭示 UC8151 LUT
 * 压缩格式（每组 6B = 首字节 4×2bit 相电平 + 4 相长 20ms/单位
 * + 重复数，实测 710ms@32 单位吻合），启用 T1 反向预脉冲
 * （5×20ms）+ T3 主驱动（0x28）：时钟生效（950ms 实测 =
 * (5+40)×20+开销）但残影与单相版相同 —— 预脉冲无效，本屏
 * 残影为单相快刷单次翻转的固有特性（GxEPD2 生态同结论：fast
 * partial 必有 ghosting，靠定期全刷清除），回退单相并保养阈值
 * 8→4（见 desc.partial_count_full_refresh） */
#define LUT_TX19 0x28
static const uint8_t LUT_VCOM_DC[44] = {
    0x00, LUT_TX19, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00,
};
static const uint8_t LUT_WW[42] = {
    0x00, LUT_TX19, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t LUT_BW[42] = {
    0x80, LUT_TX19, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t LUT_WB[42] = {
    0x40, LUT_TX19, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t LUT_BB[42] = {
    0x00, LUT_TX19, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* UC8151D 局刷 init：RST + REG LUT 配置 + 上电（GxEPD2 _Init_Part
 * 净化版：跳过被立即覆盖的 PSR 0x1F/CDI 0x97 中间写，寄存器
 * 终态等价）。s_ready 不置位 —— 局刷自含，全刷路径不受扰 */
static int uc_init_part(void)
{
    digitalWrite(EPD_RESET_PIN, HIGH);
    delay(200);
    digitalWrite(EPD_RESET_PIN, LOW);
    delay(g_panel_wft0290.rst_pulse_ms);
    digitalWrite(EPD_RESET_PIN, HIGH);
    wait_idle_level(g_panel_wft0290.busy_level, 5000);

    epd_cmd(0x00);
    epd_dat(0xBF);                    /* REG LUT（bit5=1）+ 128x296 */
    epd_cmd(0x61);
    epd_dat((uint8_t)g_panel_wft0290.panel_w);
    epd_dat((uint8_t)(g_panel_wft0290.panel_h >> 8));
    epd_dat((uint8_t)(g_panel_wft0290.panel_h & 0xFF));
    epd_cmd(0x82);                    /* VCOM_DC setting */
    epd_dat(0x08);
    epd_cmd(0x50);
    epd_dat(0x17);                    /* CDI 局刷档（GxEPD 原注 VBDF 17） */
    epd_cmd(0x20); epd_write_buf(LUT_VCOM_DC, sizeof(LUT_VCOM_DC));
    epd_cmd(0x21); epd_write_buf(LUT_WW, sizeof(LUT_WW));
    epd_cmd(0x22); epd_write_buf(LUT_BW, sizeof(LUT_BW));
    epd_cmd(0x23); epd_write_buf(LUT_WB, sizeof(LUT_WB));
    epd_cmd(0x24); epd_write_buf(LUT_BB, sizeof(LUT_BB));

    epd_cmd(0x04);                    /* Power on */
    wait_idle_level(g_panel_wft0290.busy_level,
                    g_panel_wft0290.busy_timeout_ms);
    return 0;
}

/* —— ops —— */

static int panel_init(void)
{
    /* bring-up 第三轮：通电自检 → UC8151D init + 白屏试刷（屏白 =
     * 判定 + TRES 序列双实锤，UI 全刷随即接管）；自检异常 →
     * fail-safe 返回 -1（epd_driver_init LOG_E 退出） */
    if (detect_alive() != 0) return -1;

    if (uc_init() != 0) return -1;

    Serial.printf("[W29-DIAG] first white refresh (128x296 BW)...\n");
    const uint32_t t0 = millis();
    uc_refresh_bw(NULL);               /* 白屏试刷 */
    Serial.printf("[W29-DIAG] white refresh took %ums (fill desc.full_ms)\n",
                  (unsigned)(millis() - t0));
    return 0;
}

static int panel_full_refresh(const uint8_t *frame)
{
    /* frame：BW 单平面整帧（bit=1 白），plane_count=1 连续布局。
     * 无状态：每次完整 RST+PSR/TRES/CDI 重配（UC8151D 地址计数器
     * 靠 RST 归零，分辨率寄存器靠显式重发） */
    if (!s_ready && uc_init() != 0) return -1;
    return uc_refresh_bw(frame);
}

static int panel_write_full(const uint8_t *frame)
{
    /* 竖屏原始帧直通全刷（epd_full_refresh / LAN 语义）：frame=NULL
     * 清白。注：本路径不维护 prev 帧一致性，调用方须强制下一次全刷
     * （ui_force_full_refresh_next 已保证） */
    if (!frame) {
        if (!s_ready && uc_init() != 0) return -1;
        return uc_refresh_bw(NULL);
    }
    return panel_full_refresh(frame);
}

static int panel_partial(const uint8_t *prev, const uint8_t *new_,
                         uint8_t passes)
{
    /* Plan B（DEPG0370 先例移植）：无窗口整屏双 RAM 差分 —— 不发
     * 0x91/0x90，整屏写 0x10 旧帧 + 0x13 新帧，COG 按 RAM 差分只
     * 驱动变化像素。UC 族 partial window 三组参数在 UC8253 实证
     * 不能干净刷白（panel_depg0370 注释），同族规避；代价整屏
     * 9.4KB SPI @4MHz ≈ 20ms 可忽略。passes 双刷：REG LUT 驱动力
     * 弱于全刷 LUT，单次不彻底时第二次 0x12 再驱动（DEPG 同款） */
    if (passes < 1) passes = 1;
    const size_t plane_bytes =
        (size_t)(g_panel_wft0290.panel_w / 8) * g_panel_wft0290.panel_h;

    if (uc_init_part() != 0) return -1;

    epd_cmd(0x10);                    /* previous plane（整屏） */
    epd_write_buf(prev, plane_bytes);
    epd_cmd(0x13);                    /* current plane（整屏） */
    epd_write_buf(new_, plane_bytes);

    const uint32_t t0 = millis();
    for (uint8_t p = 0; p < passes; p++) {
        epd_cmd(0x12);
        wait_refresh_done();
    }
    Serial.printf("[W29-DIAG] partial %ux0x12 took %ums\n",
                  passes, (unsigned)(millis() - t0));

    epd_cmd(0x02);                    /* Power off */
    wait_idle_level(g_panel_wft0290.busy_level, 1000);
    return 0;
}

static void panel_power_off(void)
{
    /* GxEPD2 _PowerOff 忠实：0x02 关高压 rails（VCI 3.3V 保持供电）。
     * 完成后归零 s_ready —— 下次刷新完整重配 */
    if (!s_ready) return;
    epd_cmd(0x02);
    wait_idle_level(g_panel_wft0290.busy_level, 1000);
    s_ready = false;
}

static void panel_deep_sleep(void)
{
    /* 深睡 0x07/0xA5（~µA 级），RST 硬复位唤醒 + uc_init 重初始化 */
    epd_cmd(0x07);
    epd_dat(0xA5);
    delay(200);
    s_ready = false;
}

/* —— desc 注册（三轮实锤回填：controller=UC8151D、busy_level=0、
 * full_ms=3400 实测；GxEPD2_290_T5D 实测口径 power_on 97/全刷
 * 3251/局刷 705ms，本屏白屏试刷复测校准 —— */
const epd_panel_desc_t g_panel_wft0290 = {
    .name       = "wft0290_bw",
    .controller = EPD_CTRL_UC8151,  /* 三轮实锤（GDEW029T5D 同族，
                                     * WFT0290CZ10 FPC 对照 + TRES 修复） */
    .panel_w    = 128,
    .panel_h    = 296,
    .gfx_rotation = 0,       /* 竖屏持机（gfx 128x296，LAYOUT_TINY） */
    .color_mode = EPD_COLOR_BW,
    .plane_count = 1,
    .palette    = {
        /* BW 语义同 panel_depg0370（§9.3/§9.4：bit0=B/W 平面白位） */
        [EPD_GFX_WHITE]  = 0x01,
        [EPD_GFX_BLACK]  = 0x00,
        [EPD_GFX_ACCENT] = 0x00,   /* BW 退化：强调色降级黑（§9.2） */
        [EPD_GFX_AUX]    = 0x00,
    },
    .fb_location = EPD_FB_AUTO,    /* 4.7KB x2 帧+画布 全 SRAM（§10.1） */
    .rst_pulse_ms = 10,            /* RST LOW 脉宽（判定/init 实测口径） */
    .busy_level = 0,               /* BUSY=LOW 忙（UC 家族，三轮实测 +
                                    * GxEPD2_290_T5D 构造 LOW 双证） */
    .busy_timeout_ms = 5000,       /* GxEPD2 全刷 3251ms + 余量 */
    .power_on_ms  = 100,           /* GxEPD2 实测 96.6ms */
    .power_off_ms = 50,            /* GxEPD2 实测 39.7ms */
    .full_ms    = 3400,            /* 本屏实测 3370ms（二轮，同 LUT） */
    .partial_ms = 900,             /* 实测单刷 Tx19=0x28：0x12 active
                                    * 869ms（0x20 版 710ms 实证时长随
                                    * 相长线性，GxEPD2 同族 705@0x20
                                    * 吻合），取整 + 余量 */
    .partial_enabled = true,        /* REG LUT 局刷已移植（本文件
                                    * uc_init_part + panel_partial） */
    .passes     = 1,               /* 终值（2026-08-26 对照实验实锤）：
                                    * 双刷时代残影 + 「翻页短暂回退」
                                    * 双症状同源于第二遍同向过驱动
                                    * （单相 LUT 无反向修正，粒子过冲
                                    * 回弹）；单刷后两症俱消。
                                    * 注：epd_gfx_flush_window 已改读
                                    * 本字段（原硬编码 2） */
    .partial_count_full_refresh = 4, /* 局刷 4 次强制全刷保养：残影为
                                    * 单相快刷固有特性（四相预脉冲
                                    * 实验证伪，见 LUT 注释），8→4
                                    * 加频清除（全刷 3.4s 洗净实测） */
    .window_8align = true,         /* gfx 侧窗口 8 对齐约束（x 字节轴） */
    .ops = {
        .init         = panel_init,
        .full_refresh = panel_full_refresh,
        .write_full   = panel_write_full,
        .partial      = panel_partial,   /* REG LUT 快刷（本轮移植） */
        .power_off    = panel_power_off,
        .deep_sleep   = panel_deep_sleep,
        .probe        = NULL,      /* epd_driver 诊断 0x71 FLG 已覆盖
                                    * （UC8151D 应答者，实测 0x13） */
        .write_planes = NULL,      /* BW 单平面，无多平面入口 */
    },
};
