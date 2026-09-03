/**
 * @file panel_opm021eb.cpp
 * @brief 2.13" 122x250 BW（OPM021EB 电子标签）面板单元（L0）
 *        —— UC8151D 序列（WFT0290 GxEPD2_290_T5D 路线适配）
 *
 * 面板：OPM021EB（FPC 标 OPM021EB_V1.0），2.13" 122x250 BW 电子
 * 标签屏（ESL 源），24P FPC。
 *
 * bring-up 轮次：
 *   一轮 SSD1680 假设证伪（2026-08-30 真机日志）：RST 脉冲后 BUSY
 *     忙(LOW)→闲(HIGH) 往返可见（COG 在位），但静置空闲电平 HIGH
 *     5/5 采样（SSD16xx 新代应为 LOW）+ 0x2F 版本读 0xFF/0x00 不稳
 *     （无 SSD 系状态命令）——与 WFT0290 一轮证伪同款证据链，
 *     家族改判 UC 系（忙 LOW / 空闲 HIGH）。
 *   二轮（本版）：UC8151D 序列（WFT0290 三轮定稿移植）——PSR 0x1F
 *     + TRES 显式 122x250 + CDI 0x97；122 属 PSR RES 档 128x250 类
 *     （RAM 16B/行组织，122px 有效 + 6 位无源极线）。
 *
 * 序列（panel_wft0290.cpp 同源，GxEPD2_290_T5D 忠实）：
 *   init  = RST 200/10/200 + PSR(0x00: 0x1F，OTP LUT + TRES 自定义)
 *           + TRES(0x61: source 122 / gate 250) + CDI(0x50: 0x97)；
 *   刷新  = 0x04 上电(BUSY) → 0x13 写 4,000B（16B/行 x 250）→
 *           0x12 刷新(BUSY ~2.5s 估) → 0x02 关电(BUSY)；
 *   深睡  = 0x07/0xA5（RST 硬复位唤醒）；BUSY=LOW 忙（UC 家族，
 *   空闲 HIGH，一轮实测 + WFT0290 GxEPD2_290_T5D 构造双证）。
 *
 * 局刷（WFT0290 REG LUT 方案移植）：PSR 0xBF + 0x82/0x08 + CDI
 *   0x17 + 0x20~0x24 五张 LUT；数据走无窗口整屏双 RAM 差分
 *   （0x10 旧帧 + 0x13 新帧，COG 全屏差分只驱动变化像素）。
 *
 * 几何：122x250 竖屏 rotation=0（用户选型标签屏竖持 → gfx 122x250，
 * LAYOUT_TINY 档，当前最小屏）。RAM 物理行宽 16 字节（128px），
 * 122px 有效——帧缓冲 stride 同 16B/行直通行末 6 位无源极不显示
 * （epd_driver panel_stride 已修 (w+7)/8，本屏为首个非 8 整除宽
 * 面板；GxEPD2 WI=(W+7)/8 同款处理）。
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
extern const epd_panel_desc_t g_panel_opm021eb;

static bool s_ready = false;   /* uc_init 完成（power_off/deep_sleep 归零） */
static bool s_cog_ready = false;  /* COG 已初始化刷（deep_sleep 归零）：
 * RST 后首次 0x12 前的 RAM 写入无效（十三轮实锤），局刷路径凭此
 * 标志判断 COG 是否需先走完整 uc_init */

/* 八轮定稿（2026-08-30）：COG 行宽 = 15 整字节（120 位/行）。
 * 九轮 122 位/行位流证伪（花屏/斜纹回归）。完整证据链：
 *   二轮 16B/行直写 → 斜纹（行宽 15B，帧 16B 逐行漂 1B）
 *   三~五轮 TRES source=128 → 白屏（源极绑定 122，超限空转）
 *   七轮 0x21/0x44 Bypass 刷黑成功 → 驱动层通，锁定 RAM 写入层
 *   六轮 15B 重排但 250 段事务 → 白屏（CS↑ 重置地址计数器）
 *   八轮 15B/行重排 + 单 CS 事务连续流 → 屏亮词条页（定稿）
 * 十轮删 Bypass 块 → 白屏（0x21 序列副作用实锤，块保留）
 * 代价：帧行第 16 字节（UI x120..127）丢弃，屏右缘 x120..121
 * 两列不刷新（保持前次状态），UI 排版需避开右缘 2px */
#define K_ROW_BYTES 15   /* COG 行宽（floor(panel_w/8)=120 位，八轮定稿） */

/* —— UC8151D init（GxEPD2_290_T5D._InitDisplay 忠实，TRES 换 122x250）——
 * demo 板 EN 脚（VCI 使能）为 DEPG demo 板专属，本驱动板 VCI 直供 */
static int uc_init(void)
{
    digitalWrite(EPD_RESET_PIN, HIGH);
    delay(200);
    digitalWrite(EPD_RESET_PIN, LOW);
    delay(g_panel_opm021eb.rst_pulse_ms);
    digitalWrite(EPD_RESET_PIN, HIGH);
    bus_wait_idle(&g_panel_opm021eb, 5000);  /* boot 自检 */

    /* PSR：单字节 0x1F = OTP LUT + TRES 自定义 + 默认扫描（UC8253 的
     * 双字节 PSR 第二字节会被 UC8151D 误锁存，WFT0290 二轮花屏根因） */
    bus_cmd(0x00);
    bus_dat(0x1F);

    /* 实验（四轮 2026-08-30）：三轮同时加 TRES 128 + 0x11/0x03 双变
     * 量致白屏，单变量回退 0x11（疑 POR 默认非 0x03 且改写引入偏差；
     * 二轮无 0x11 时斜纹=行宽错位非入口方向问题） */

    /* TRES（六轮）：回 HR=122（五轮证伪 source=128 全白，仅 122 有
     * 显示）+ gate=250；行宽由 COG 按 floor 15B 解析，数据侧重排适配 */
    bus_cmd(0x61);
    bus_dat((uint8_t)g_panel_opm021eb.panel_w);               /* 122 */
    bus_dat((uint8_t)(g_panel_opm021eb.panel_h >> 8));        /* 250 高位 */
    bus_dat((uint8_t)(g_panel_opm021eb.panel_h & 0xFF));      /* 250 低位 */

    /* CDI：VCOM 与数据间隔 —— 二十二轮 0x97→0x17 试验无改善后回滚
 * （OTP 波形模式下驱动参数寄存器被忽略，能量烧死在 OTP 里；
 * GxEPD2 原注 WBmode:VBDF 17|D7 VBDW 97 VBDB 57） */
    bus_cmd(0x50);
    bus_dat(0x97);
 
    /* 二十一轮（2026-08-31）：温度传感器源切内部 —— 实测对显示质量
 * 无改善（屏体中部微灰为 ESL 使用史老化，非温度档错），但 ESL 拆机
 * 屏外挂 LM75 已裁（在原主板而非 FPC），内部源是正确防御，保留 */
    bus_cmd(0x1C);
    bus_dat(0x80);                    /* TSE：内部温度传感器 */

    /* 十三轮（2026-08-30）初始化刷：RST 后首次 0x12 完成内部初始化
 * （温度/boost/波形表），初始化前的 RAM 写入（0x10/0x13）全部无效
 * ——统一解释八轮显示（Bypass 块 0x12 充当初始化）、十/十二轮白屏
 * （无 0x12 即写 RAM）、局刷 50ms 空转（uc_init_part 每次硬 RST）。
 * 此处不写 RAM 空刷一次（刷出 RAM 默认值，屏闪黑 ~3s，boot 一次） */
    DIAG_LOG("init refresh (first 0x12 after RST)...");
    bus_cmd(0x04);
    bus_wait_idle(&g_panel_opm021eb,
                    g_panel_opm021eb.busy_timeout_ms);
    bus_cmd(0x12);
    bus_wait_busy(&g_panel_opm021eb, g_panel_opm021eb.busy_timeout_ms);
    bus_cmd(0x02);
    bus_wait_idle(&g_panel_opm021eb, 1000);

    s_ready = true;
    s_cog_ready = true;
    return 0;
}

/* RAM 整帧写入（八轮定稿）：单 CS 事务 + 15B/行重排流 —— 帧行
 * 16B stride 逐行发前 15B（丢弃第 16 字节），总 3,750B；
 * frame=NULL 时同事务全量写白 */
static void write_ram_frame(const uint8_t *frame)
{
    const size_t stride = (size_t)((g_panel_opm021eb.panel_w + 7) / 8);
    bus_dat_begin();                          /* 单 CS 事务（陷阱②：CS↑
                                              * 重置地址计数器，勿分段） */
    if (frame) {
        for (int y = 0; y < g_panel_opm021eb.panel_h; y++)
            for (int b = 0; b < K_ROW_BYTES; b++)
                bus_dat_put(frame[y * stride + b]);
    } else {
        const size_t ram_bytes =
            (size_t)K_ROW_BYTES * g_panel_opm021eb.panel_h;
        for (size_t i = 0; i < ram_bytes; i++) bus_dat_put(0xFF);
    }
    bus_dat_end();
}

/* B/W 整屏双写 + 全刷（三十九轮定稿）：0x04 上电 → 0x10 写 DTM1
 * （old）+ 0x13 写 DTM2（new，同帧数据）→ 0x12 → 0x02。双写是
 * 三十八轮探针（probe_opm_dram H1 组）定案：本 COG 的 DRF 波形
 * 引擎需要 DTM1/DTM2 同时有效（差分驱动语义，WFT0290/UC8253
 * 家族同源惯例）——只写 0x13 时 DTM1 停在旧帧，屏显恒滞后一帧
 * （33~37 轮全部滞后/旧图案的统一根因）；boot 首屏正确源于 RST
 * 后首次 0x12 的内部同步。frame 约定同 DEPG0370：竖屏整帧
 * 15B/行（write_ram_frame 重排口径） */
static int uc_refresh_bw(const uint8_t *frame /* NULL = 全白 */)
{
    bus_cmd(0x04);                     /* Power on（GxEPD2 先上电再写 RAM） */
    bus_wait_idle(&g_panel_opm021eb,
                    g_panel_opm021eb.busy_timeout_ms);

    bus_cmd(0x10);                     /* DTM1 old（同帧，差分引擎基准） */
    write_ram_frame(frame);
    bus_cmd(0x13);                     /* DTM2 new（地址计数器 RST 归零） */
    write_ram_frame(frame);

    /* 二十三轮双全刷（0x12×2 物理叠加提对比度）终判：无改善 —— 真机
 * 白屏纯色实测四周白/中间微灰（位置相关，与内容/驱动无关），
 * ESL 使用史中部内容区老化实锤，双刷对老化无救，回滚单刷省 3s */
    bus_cmd(0x12);                     /* display refresh（无哑字节） */
    bus_wait_busy(&g_panel_opm021eb, g_panel_opm021eb.busy_timeout_ms);

    bus_cmd(0x02);                     /* Power off */
    bus_wait_idle(&g_panel_opm021eb, 1000);
    return 0;
}

/* —— 打断法快刷翻案史（三十一~四十一轮）——
 * 31/32 轮（单写 0x13 时代）：POF 打断流程层可行但真机翻词滞后
 * 一帧，误判“断点续刷型 COG / 波形时间恒定 2965ms 数学无解”证伪。
 * 38 轮定案滞后真因 = DTM1(0x10) 未写（双 RAM 差分引擎需双有效，
 * 单写屏显恒滞后一帧）——误判根基拔除。40 轮探针
 * （probe_opm_dw_abort）双写+打断组合实测：X=250/500/1000/2000ms
 * 四档条带图全部逐帧正确显示当前帧（无滞后），POF 后 boost 余量
 * 驱动使实际对比度远超标称打断点。“波形时间恒定”推论作废。
 * 快刷终局重写：REG LUT 硅锁 + OTP 无快档仍在（无寄存器级快刷），
 * 但打断法在双写序列下成立（全刷波形截短，ED057TC1 同源） */
#define K_ABORT_MS 250                 /* 打断点：波形执行 0.25s 处 POF。
                                        * 四十三轮定档：1000/500/250ms
                                        * 阶梯降档真机文字均清晰，取
                                        * 探针 40 轮最低可辨档（~0.65s/帧）*/

/* —— ops —— */

static int panel_full_refresh(const uint8_t *frame);
static int panel_write_full(const uint8_t *frame);

static int panel_init(void)
{
    /* bring-up 二轮：通电自检（UC 家族口径）→ UC8151D init + 白屏
     * 试刷（屏白 = 家族判定 + TRES 序列双实锤，UI 全刷随即接管）；
     * 自检异常 → fail-safe 返回 -1（epd_driver_init LOG_E 退出） */
    if (bus_detect_alive(&g_panel_opm021eb) != 0) return -1;
    if (uc_init() != 0) return -1;

    /* 十二轮实验（2026-08-30）：0x21 序列二分定位 —— 八轮块含
 * 0x21/0x44→0x04→0x12→0x21/0x00→delay(6s) 多成分，若全量搬进
 * 局刷则每次翻词 +9s 不可用；本轮全刷/局刷两路径均只保留纯
 * 0x21 寄存器写（去掉刷新循环与延时）——若 boot 词条页仍显示
 * 且翻页局刷生效，则必要成分 = 0x21 写入本身（µs 级，可定稿） */
    DIAG_LOG("0x21 pattern (pure reg write)");
    bus_cmd(0x21);
    bus_dat(0x44);
    bus_cmd(0x21);
    bus_dat(0x00);
    DIAG_LOG("first white refresh (122x250 BW)...");
    #if INKWORD_EPD_DIAG
    const uint32_t t0 = millis();
    uc_refresh_bw(NULL);               /* 白屏试刷 */
    DIAG_LOG("white refresh took %ums (fill desc.full_ms)",
             (unsigned)(millis() - t0));
    #endif
    return 0;
}

static int panel_full_refresh(const uint8_t *frame)
{
    /* frame：BW 单平面整帧（bit=1 白），plane_count=1 连续布局，
     * 行宽 16B（122px 向上取整，epd_driver panel_stride 同源）。
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

/* 打断法快刷（四十一轮，2026-09-01）：双写序列（同 39 轮口径）+
 * 0x12 波形执行 K_ABORT_MS 处 0x02 POF 打断。40 轮探针实证四档
 * 全部逐帧正确显示当前帧；POF 后寄存器不丢（31 轮 POF/PON 轮转
 * 实证），每帧 PON 重升压。打断截断清屏相位会积累残影 —— 由
 * refresh_scheduler 阈值保养全刷（局刷 8 次强制 1 次完整全刷）清，
 * 架构现成。prev 不参与（差分由 COG DTM1 承担，同帧双写） */
static int panel_partial(const uint8_t *prev, const uint8_t *new_,
                         uint8_t passes)
{
    (void)prev;
    if (passes < 1) passes = 1;
    if (!s_ready && uc_init() != 0) return -1;

    #if INKWORD_EPD_DIAG
    const uint32_t t0 = millis();
    #endif
    for (uint8_t p = 0; p < passes; p++) {
        bus_cmd(0x04);                 /* PON：POF 态后重升压 */
        bus_wait_idle(&g_panel_opm021eb,
                     g_panel_opm021eb.busy_timeout_ms);
        bus_cmd(0x10);                 /* DTM1 old（同帧，差分基准） */
        write_ram_frame(new_);
        bus_cmd(0x13);                 /* DTM2 new */
        write_ram_frame(new_);
        bus_cmd(0x12);
        delay(K_ABORT_MS);             /* 波形执行 Xms 处 */
        bus_cmd(0x02);                 /* POF 打断（不等波形完成） */
        bus_wait_idle(&g_panel_opm021eb, 1000);
    }
    DIAG_LOG("partial abort %ux%ums took %ums",
             passes, (unsigned)K_ABORT_MS,
             (unsigned)(millis() - t0));
    return 0;
}

static void panel_power_off(void)
{
    /* P2d：UC8151D 系关电收敛 epd_bus（两家面板 byte 级一致） */
    bus_uc_power_off(&g_panel_opm021eb, &s_ready);
}

static void panel_deep_sleep(void)
{
    bus_uc_deep_sleep(&s_ready);
    s_cog_ready = false;    /* 唤醒后首刷走全刷路径重建初始化态 */
}

/* —— desc 注册（二轮实锤回填：UC 家族 busy_level=0；controller
 * 初判 UC8151D（0x71 FLG 稳定 0x13 = WFT0290 同族应答值；
 * IL0373 系兼容）；时序：全刷/白屏试刷二轮实测，局刷为 WFT0290
 * 同 LUT 口径（869ms）待本屏翻页实测校准 —— */
const epd_panel_desc_t g_panel_opm021eb = {
    .name       = "opm021eb_bw",
    .controller = EPD_CTRL_UC8151,  /* 一轮 SSD1680 证伪改判（BUSY
                                     * 空闲 HIGH + 0x2F 无应答，日志
                                     * 实锤；UC8253/IL0373 同模型） */
    .panel_w    = 122,
    .panel_h    = 250,
    .gfx_rotation = 0,       /* 竖屏持机（gfx 122x250，LAYOUT_TINY） */
    .color_mode = EPD_COLOR_BW,
    .plane_count = 1,
    .palette    = {
        /* BW 语义同 panel_depg0370（§9.3/§9.4：bit0=B/W 平面白位） */
        [EPD_GFX_WHITE]  = 0x01,
        [EPD_GFX_BLACK]  = 0x00,
        [EPD_GFX_ACCENT] = 0x00,   /* BW 退化：强调色降级黑（§9.2） */
        [EPD_GFX_AUX]    = 0x00,
    },
    .accent_rgb = 0x000000,    /* BW 无第三色，LAN 调色板不注入 */
    .fb_location = EPD_FB_AUTO,    /* 4,000B x2 帧 + 画布 4,000B
                                   * ≈ 12KB 全 SRAM（§10.1 余量最宽） */
    .rst_pulse_ms = 10,            /* RST LOW 脉宽（WFT0290 实测口径） */
    .busy_level = 0,               /* BUSY=LOW 忙（UC 家族，一轮实测
                                    * 空闲 HIGH + WFT0290 双证） */
    .busy_timeout_ms = 5000,       /* 实测忙 2,970ms（二轮白屏
                                    * 试刷）+ 1.7 倍余量 */
    .power_on_ms  = 100,           /* WFT0290 实测 96.6ms */
    .power_off_ms = 50,            /* WFT0290 实测 39.7ms */
    .full_ms    = 3200,            /* 二轮实测 3,117ms（含上电/
                                    * 关电，波形 2,970ms 稳定复现
                                    * 四次）+ 余量 */
    .partial_ms = 650,             /* 打断法快刷：K_ABORT_MS=250 +
                                    * 传输/POF 开销（四十三轮最低档） */
    .partial_enabled = true,       /* 打断法快刷（四十三轮定档）：双写
                                    * + 250ms POF 打断 ~0.65s/帧；真机
                                    * 1000/500/250 三档文字均清晰 */
    .passes     = 1,
    .partial_count_full_refresh = 4, /* 局刷计数保养（OTP 全刷波形
                                    * 自带清屏相位，残影轻，4 次保守） */
    .window_8align = true,         /* gfx 侧窗口 8 对齐约束（x 字节轴） */
    .ops = {
        .init         = panel_init,
        .full_refresh = panel_full_refresh,
        .write_full   = panel_write_full,
        .partial      = panel_partial,   /* 转发双写全刷（三十九轮） */
        .power_off    = panel_power_off,
        .deep_sleep   = panel_deep_sleep,
        .probe        = NULL,       /* epd_driver 诊断 0x71 FLG 已覆盖
                                    * （UC 家族应答者） */
        .write_planes = NULL,       /* BW 单平面，无多平面入口 */
        .diag         = bus_diag_uc, /* T1.8：UC 族 FLG 0x71 双读 */
    },
};
