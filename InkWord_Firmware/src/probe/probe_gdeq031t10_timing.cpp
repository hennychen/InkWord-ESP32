/**
 * @file probe_gdeq031t10_timing.cpp
 * @brief 3.1" 屏刷新时长对照探针（env:gdeq031t10-timing，2026-09-19）
 *
 * 起因：3.1" 真机首刷日志显示局刷忙窗稳定 3088ms，而规格标称
 * 全刷 3s／快刷 1s／局刷 0.5s ——「局刷」跑成了全刷时长。
 * 库内官方同类驱动 gdeq/GxEPD2_310_GDEQ031T10 的实测常量是
 * full_refresh_time=1100（作者实测 1015ms）、partial_refresh_time=700
 * （650ms）：同一块屏两套序列差 3 倍，说明差异在驱动侧而非屏体。
 *
 * 按项目既有「开源库标准驱动探针对照法」做 A/B + 归因臂 C/D/E/G：
 *   A 臂 = 官方 GxEPD2_310_GDEQ031T10（PSR 两字节 0x1F,0x0D）
 *   B 臂 = 本项目 GxEPD2_gdeq031t10 demo 序列（PSR 单字节 0x1F）
 *   C/D/E 臂（2026-09-19 已跑完，代码按结论撤下）= 官方序列逐字节
 *     复刻后每次只翻一个开关：PSR 第二字节、是否写 0x10 旧帧 RAM、
 *     0x12 后杂散数据字节、0x04 发两次、复位与断电时机（冷热态）
 *     —— 全部读数 3088ms 不变，即差异不在寄存器取值也不在电源时序
 *   G 臂 = 同款复刻 ± E0=0x02/E5=0x5A（官方 useFastFullUpdate 的
 *     强制内部温度），最后一项候选
 * 计时口径：写 RAM 后夹住 refresh() 取墙钟——类内部 _waitWhileBusy
 * 一 BUSY 回闲就返回，故墙钟≈真实忙窗；若忙窗超过类内超时值，
 * GxEPD2 会打 "timed out" WARNING 且墙钟被截断，读数需连同该告警
 * 一起看（A 臂超时上限 1100/700ms，B 臂 3000/500ms；D 臂用 8000ms
 * 上限单独量 0x12 净忙窗）。
 * 首轮实测（2026-09-19）：A 全刷 1018/1055ms、A 局刷 689ms；
 * B 全刷 3130/3166ms、B 快刷 1060ms、B 局刷(1 pass) 652ms；C 同 B。
 * 屏上内容不重要（纯单色帧），只取时长；结论回填 desc 的
 * full_ms/partial_ms/busy_timeout_ms 与全刷波形选型。
 */
#include <Arduino.h>
#include <SPI.h>
#include "../gpio_config.h"
#include "../GxEPD2_gdeq031t10.h"
#include <gdeq/GxEPD2_310_GDEQ031T10.h>

#define FRAME_BYTES (240 * 320 / 8)     /* 9600 B，竖屏原生行主序，bit=1 白 */

static GxEPD2_310_GDEQ031T10 s_a(EPD_CS_PIN, EPD_DC_PIN, EPD_RESET_PIN, EPD_BUSY_PIN);
static GxEPD2_gdeq031t10     s_b(EPD_CS_PIN, EPD_DC_PIN, EPD_RESET_PIN, EPD_BUSY_PIN);

static uint8_t *s_white, *s_black;

static void report(const char *arm, const char *mode, uint32_t ms, int32_t spec_ms)
{
    Serial.printf("  %-2s %-10s = %5ums  (标称 %4ldms, 偏差 %+4d%%)%s\n",
                  arm, mode, (unsigned)ms, (long)spec_ms,
                  spec_ms > 0 ? (int)((ms * 100) / (uint32_t)spec_ms) - 100 : 0,
                  ms == 0 ? "   ← 无忙窗！" : "");
}

/* 官方臂：写单色新帧 → 全刷/局刷，墙钟计时 */
static uint32_t arm_a_refresh(bool partial, uint8_t fill)
{
    s_a.writeScreenBuffer(fill);
    uint32_t t0 = millis();
    s_a.refresh(partial);
    return millis() - t0;
}

/* 本项目臂：demo 真全刷（无窗口写 0x13 → refresh(false)） */
static uint32_t arm_b_full(const uint8_t *fb)
{
    s_b.demoWriteFull(fb);
    uint32_t t0 = millis();
    s_b.refresh(false);
    return millis() - t0;
}

/* 本项目臂：生产局刷等价路径（硬复位 → 局刷初始化 → 双 RAM →
 * 0x04/0x12×passes/0x02）；updateDemoPartial 内部自带 [EPD] 时长打印。
 * passes 参数化：量产 epd_gfx_flush 传 desc.passes（本屏 2），单遍口径
 * 只用于对比，别把 1 pass 的读数当生产局刷时长 */
static void arm_b_partial(const uint8_t *prev, const uint8_t *next, uint8_t passes)
{
    s_b.hwReset();
    s_b.initPartialDemo();
    s_b.demoWriteDualNoWindow(prev, next);
    s_b.updateDemoPartial(passes);
}

/* ---- 归因臂公共设施：用库自带底层 SPI 原语（protected → using 暴露）
 * 自己拼官方序列，逐项排除差异。 ---- */
class TuningDriver : public GxEPD2_gdeq031t10
{
  public:
    using GxEPD2_gdeq031t10::GxEPD2_gdeq031t10;
    using GxEPD2_EPD::_writeCommand;
    using GxEPD2_EPD::_writeData;
    using GxEPD2_EPD::_startTransfer;
    using GxEPD2_EPD::_transfer;
    using GxEPD2_EPD::_endTransfer;
    using GxEPD2_EPD::_waitWhileBusy;
};

static TuningDriver s_d(EPD_CS_PIN, EPD_DC_PIN, EPD_RESET_PIN, EPD_BUSY_PIN);

static void stream_ram(TuningDriver &d, uint8_t cmd, const uint8_t *fb)
{
    d._writeCommand(cmd);
    d._startTransfer();
    for (uint32_t i = 0; i < FRAME_BYTES; i++) d._transfer(fb[i]);
    d._endTransfer();
}

/* F/G 臂公共体：官方真序逐字节复刻——
 *   PSR(0x1e,0x0d)+PSR(0x1f,0x0d) → 写 RAM(0x10 旧 + 0x13 新) → CDI 0x97
 *   →〔fast_temp 时补 E0=0x02 + E5=0x5A〕→ 0x04 上电 → 0x12 → 量净忙窗
 * 即 RAM 写在 0x04 之【前】（本项目 demo 序列写在其后），且每次刷新前
 * 重发那对 PSR。
 *
 * 已用量程（2026-09-19 首轮，同序列单开关矩阵，全部 3088ms 无差异）：
 *   D：PSR 1/2 字节、写不写 0x10、0x12 后杂散 0x00、0x04 发两次
 *   E：复位后连刷 / 断电后不复位重新上电（冷热态）
 * 剩余唯一未排除项＝官方 useFastFullUpdate 的 E0/E5，即本轮 G 组。 */
static uint32_t arm_official_full(TuningDriver &d, bool fast_temp,
                                  const uint8_t *prev, const uint8_t *next)
{
    d.hwReset();
    d._writeCommand(0x00);
    d._writeData(0x1E);
    d._writeData(0x0D);
    d._writeCommand(0x00);
    d._writeData(0x1F);
    d._writeData(0x0D);
    stream_ram(d, 0x10, prev);
    stream_ram(d, 0x13, next);
    d._writeCommand(0x50);
    d._writeData(0x97);
    if (fast_temp) {
        d._writeCommand(0xE0);
        d._writeData(0x02);            /* CCSET: TSFIX */
        d._writeCommand(0xE5);
        d._writeData(0x5A);            /* 强制内部温度，官方 1015ms 口径 */
    }
    d._writeCommand(0x04);
    d._waitWhileBusy("F_powon", 100);
    d._writeCommand(0x12);
    uint32_t t0 = millis();
    d._waitWhileBusy("F_refresh", 8000);
    uint32_t ms = millis() - t0;
    d._writeCommand(0x02);
    d._waitWhileBusy("F_powoff", 100);
    return ms;
}

void setup()
{
    Serial.begin(115200);
    delay(3000);
    Serial.println("\n### GDEQ031T10 3.1\" refresh timing 归因探针 ###");
    Serial.println("  对照：官方臂 A vs 本项目臂 B，G 臂±E0/E5 定因");
    Serial.println("  标称：全刷 3000 / 快刷 1000 / 局刷 500 ms");

    s_white = (uint8_t *)malloc(FRAME_BYTES);
    s_black = (uint8_t *)malloc(FRAME_BYTES);
    if (!s_white || !s_black) { Serial.println("malloc 失败"); while (true) delay(1000); }
    memset(s_white, 0xFF, FRAME_BYTES);
    memset(s_black, 0x00, FRAME_BYTES);

    SPI.begin(EPD_SCK_PIN, -1, EPD_MOSI_PIN, EPD_CS_PIN);
    pinMode(EPD_BUSY_PIN, INPUT);

    /* ---- A 臂：库内官方驱动（ live 参照，首轮已多轮复测，此处各取一次）---- */
    Serial.println("\n[A] 官方 GxEPD2_310_GDEQ031T10");
    s_a.init();                             /* 内含 RST 脉冲 + _InitDisplay */
    report("A", "full", arm_a_refresh(false, 0xFF), 1100);   /* 首次含初始化 */
    report("A", "full", arm_a_refresh(false, 0x00), 1100);
    report("A", "part", arm_a_refresh(true, 0xFF), 700);
    s_a.powerOff();
    delay(500);

    /* ---- B 臂：本项目 demo 序列 ---- */
    Serial.println("\n[B] 本项目 GxEPD2_gdeq031t10（demo 序列）");
    s_b.hwReset();
    s_b.initFullDemo();
    report("B", "full", arm_b_full(s_white), 3000);
    s_b.hwReset();
    s_b.initFastDemo();
    report("B", "fast", arm_b_full(s_black), 1000);
    /* 局刷按生产 passes 口径量：量产 epd_gfx_flush 传 desc.passes=2，
     * 单遍只作对照（updateDemoPartial 自带 [EPD] 忙窗打印） */
    Serial.println("  局刷 1 pass 对照:");
    arm_b_partial(s_black, s_white, 1);
    Serial.println("  局刷 2 pass（= 量产 desc.passes 口径）:");
    arm_b_partial(s_white, s_black, 2);
    arm_b_partial(s_black, s_white, 2);

    /* ---- G 臂：官方真序复刻（RAM 写在 0x04 前 + 每次重发 PSR 对），
     *      唯一变量 = 官方 useFastFullUpdate 的 E0=0x02 + E5=0x5A。
     *      D/E 两臂（首轮，矩阵代码已按结论撤下）证明寄存器取值与
     *      冷热态均非变量，这是最后一个未排除项。 ---- */
    Serial.println("\n[G] 官方真序 ±E0/E5 强制温度（3× 差值的最后候选）");
    report("G", "no-E0E5", arm_official_full(s_d, false, s_white, s_black), 3082);
    report("G", "no-E0E5", arm_official_full(s_d, false, s_black, s_white), 3082);
    report("G", "+E0/E5",  arm_official_full(s_d, true,  s_white, s_black), 1015);
    report("G", "+E0/E5",  arm_official_full(s_d, true,  s_black, s_white), 1015);

    /* 收尾控制：官方臂再量一次，排除屏/温度随时间漂移 */
    Serial.println("\n[A2] 官方臂收尾复测（防漂移控制组）");
    s_a.init();
    report("A2", "full", arm_a_refresh(false, 0x00), 1100);
    s_a.powerOff();
    s_d.hibernate();

    Serial.println("\n=== done ===");
}

void loop() { delay(1000); }
