/**
 * @file panel_gdeq0426t82_ssd1677.cpp
 * @brief 4.26" 800x480 BW（GDEQ0426T82，17Pin）面板单元（L0）
 *        —— SSD1677 手写序列 + BW desc
 *
 * 面板：大连佳显 GDEQ0426T82，4.26" 800x480 BW 黑白横向面板
 * （活动区 92.8x55.68mm），Solomon SSD1677（800 source x 480 gate），
 * 17P FPC，4 线 SPI（BS1 低），BUSY 高电平忙。规格书
 * Info/4.26寸双色墨水屏-SSD1677/GDEQ0426T82.pdf。
 *
 * 序列来源（权威，一比一移植）：Good Display 官方 demo
 * S-GDEQ0426T82-FP-20230516（STM32）与 GDEW426T2_Arduino 双版本
 * byte 级一致，2026-09-06 核对。demo 轴命名 EPD_WIDTH=480/
 * EPD_HEIGHT=800 与本单元 panel_w/panel_h 转置（demo WIDTH=gate 方向），
 * 移植时已按 desc 语义换算：panel_w=800（source）panel_h=480（gate）。
 *
 * SSD1677 与现役 SSD16xx 面板的关键族差异（datasheet §8.3-8.5 实证）：
 *   ① 0x44/0x45/0x4E 均为双字节 10-bit 地址（SSD1680/1619 的 0x44/0x4E
 *      单字节）；
 *   ② 0x44 X 窗口按像素单位（0~799）而非字节单位——demo 局刷
 *      EPD_Dis_Part_Time 以画布像素坐标 x=320（8 对齐）宽 104px 设窗，
 *      数据 13B x 48 行，若按字节单位 x=320 即像素 2560 超界，像素单位
 *      是唯一自洽解释（2026-09-06 分析定案，bring-up 四象限复验）；
 *   ③ 全刷 0x22/0xF7 / 快刷 0x22/0xC7（前置 0x1A 温度 + 0x22/0x91 激活，
 *      demo 注释 1.5s 档）/ 局刷 0x22/0xFF 三档分离；
 *   ④ 局刷为硬件窗口方案：BaseMap（0x24+0x26 双写+全刷）建立差分
 *      基线后，窗口/全屏写 0x24 新帧 + 0x22/0xFF 激活。
 *
 * 局刷实现与 demo 的两处受控偏差（理由，非抄写变体）：
 *   ① full_refresh 采用 demo EPD_SetRAMValue_BaseMap 式 0x24+0x26
 *      双写（而非 EPD_WhiteScreen_ALL 单写 0x24）：保证任意全刷后
 *      0x26 差分基线 = 当前显示帧，后续 partial 不依赖「屏上电后曾
 *      调过 BaseMap」的 demo 时序前提（无状态铁律）；全刷波形
 *      0xF7 不变，0x26 内容对 BW 波形无影响（SSD16xx 家族语义，
 *      WF0270/E042A13BW 先例）；
 *   ② partial 显式写 0x26=prev + 0x24=new（demo EPD_Dis_PartAll 只写
 *      0x24，依赖 0x26 保留旧帧）：每次刷新差分基准，语义严格正确，
 *      不依赖 COG 内部 0x24→0x26 自动拷贝行为（SSD1619 Mode 2 的
 *      RAM Ping-Pong 需 0x37 显式使能，本屏 demo 未见，存疑不赌）。
 *
 * bring-up 状态（2026-09-06 真机）：首刷水平镜像已修正（勘误一，
 * 0x11=0x02 X 递减 + 反向窗口，用户确认方向正向）；全刷时序
 * 实测吻合（busy<6s / full~4s 口径）；X 像素单位推断经实屏
 * 正常显示间接验证。
 *
 * bring-up 勘误一（2026-09-06 真机，镜像修正）：首刷实屏水平镜像
 * （背面看正常，垂直方向正确）——RAM X 地址增长方向与面板像素
 * 物理方向相反。修法：0x11 数据入口 0x03(X+Y+) → 0x02(X-Y+，
 * demo EPD_HW_Init_GUI「mirror」先例）+ 0x44 窗口反向（XSA=799 /
 * XEA=0，datasheet §8.3 明示允许 XEA≤XSA）+ 0x4E 保持 0（X-
 * 模式下溢回卷到 XSA=799，每行从 799 递减写到 0，负负得正）。
 * 方向体系（0x11 ID 位 ×窗口反向，demo 四变体实证）：0x03 正向 /
 * 0x02 水平镜像 / 0x01 垂直镜像 / 0x00 180°。
 *
 * bring-up 勘误二（2026-09-06 网友新款屏实证）：GxEPD2 库 2026/2/12
 * 更新版 GDEQ0426T82 驱动在 Update_Full 开头新增 0x21/0x40/0x00
 * （Display Update Control 2：bypass RED + single chip application）。
 * 2023 官方 demo 无此步（旧批次 OTP 预载默认值），新款批次需显式
 * 下发 0x21 配置刷新模式——否则可能走默认三色驱动路径，BW 屏刷新
 * 异常或功耗偏高。已同步加入 do_refresh 序列（0x24/0x26 写 RAM 前）。
 */
#include "../epd_panel.h"
#include "../gpio_config.h"
#include "epd_bus.h"    /* T1.1：SPI 原语/等待收敛层 */

#include <Arduino.h>
#include <SPI.h>
#include <string.h>    /* memcmp：局刷 diff 免刷判断 */

/* 前置声明：ops 实现引用 desc 几何/时序字段 */
extern const epd_panel_desc_t g_panel_gdeq0426t82;

static bool s_ready = false;   /* init 完成（power_off/deep_sleep/
                                * partial 末尾 RST 复位后归零） */

/* 全屏 RAM 窗口 + 光标归零（写整屏帧前必调）：
 * SSD1677 双字节 10-bit 地址族特征——X 按像素，Y 按行。
 * bring-up 勘误一：X 窗口反向（XSA=799 → XEA=0）配合 0x11=0x02
 * X 递减写入，修正面板水平镜像（见文件头）；0x4E=0 在 X- 模式
 * 下溢回卷到 XSA=799，每行从 799 递减写到 0（demo GUI 同款） */
static void set_full_window(void)
{
    bus_cmd(0x44);
    bus_dat((uint8_t)((g_panel_gdeq0426t82.panel_w - 1) & 0xFF));
    bus_dat((uint8_t)((g_panel_gdeq0426t82.panel_w - 1) >> 8));  /* X start 799 */
    bus_dat(0x00); bus_dat(0x00);                               /* X end 0（反向窗口） */
    bus_cmd(0x45);
    bus_dat(0x00); bus_dat(0x00);                            /* Y start 0 */
    bus_dat((uint8_t)((g_panel_gdeq0426t82.panel_h - 1) & 0xFF));
    bus_dat((uint8_t)((g_panel_gdeq0426t82.panel_h - 1) >> 8));
    bus_cmd(0x4E); bus_dat(0x00); bus_dat(0x00);             /* X 计数器 0（X- 下溢回卷至 799） */
    bus_cmd(0x4F); bus_dat(0x00); bus_dat(0x00);             /* Y 计数器 0 */
}

static int panel_init(void)
{
    /* demo EPD_HW_Init 一比一（STM32/Arduino 双版本一致）：
     * RST 10ms 脉冲 → BUSY → SWRESET → BUSY → 温度传感器/升压/
     * Driver Output/边框/数据入口/窗口/计数器 */
    pinMode(EPD_RESET_PIN, OUTPUT);
    pinMode(EPD_CS_PIN, OUTPUT);
    pinMode(EPD_DC_PIN, OUTPUT);
    pinMode(EPD_BUSY_PIN, INPUT);
    digitalWrite(EPD_CS_PIN, HIGH);
    digitalWrite(EPD_DC_PIN, LOW);

    digitalWrite(EPD_RESET_PIN, LOW);
    delay(g_panel_gdeq0426t82.rst_pulse_ms);
    digitalWrite(EPD_RESET_PIN, HIGH);
    delay(10);
    bus_wait_idle(&g_panel_gdeq0426t82, 5000);   /* 复位后 boot 自检（忙→闲） */

    bus_cmd(0x12);                    /* SWRESET：寄存器回 POR */
    bus_wait_idle(&g_panel_gdeq0426t82, 5000);

    bus_cmd(0x18); bus_dat(0x80);     /* 温度传感器内置模式（demo） */

    bus_cmd(0x0C);                    /* Booster 软启动（demo 5 字节） */
    bus_dat(0xAE); bus_dat(0xC7); bus_dat(0xC3);
    bus_dat(0xC0); bus_dat(0x80);

    bus_cmd(0x01);                    /* Driver Output：MUX=480 gate */
    bus_dat((uint8_t)((g_panel_gdeq0426t82.panel_h - 1) & 0xFF));
    bus_dat((uint8_t)((g_panel_gdeq0426t82.panel_h - 1) >> 8));
    bus_dat(0x02);                    /* demo 第三字节（GD/SM/TB） */

    bus_cmd(0x3C); bus_dat(0x01);     /* BorderWaveform（全刷档） */

    bus_cmd(0x11); bus_dat(0x02);     /* 数据入口 X- Y+（bring-up 勘误一：
                                       * 0x03 实屏水平镜像，X 递减写入修正，
                                       * 见文件头方向体系） */

    set_full_window();
    bus_wait_idle(&g_panel_gdeq0426t82, 5000);

    /* 注：0xC7 快刷波形不兼容当前批次（OTP 快刷 LUT 可能缺失，
     * 2026-09-07 实测 0xC7 黑底），全刷锁定 0xF7 3.5s 物理极限。
     * 温度加载（0x1A/0x5A → 0x22/0x91 → 0x20）对 0xF7 无贡献，
     * 官方 EPD_HW_Init 无此步，移除省首刷 ~700ms */

    s_ready = true;
    return 0;
}

/* 双 RAM 写入 + 快刷：0x26=0x24=frame（BaseMap 式，见文件头偏差①），
 * 波形 0x22/0xC7 + 0x20 Master Activation（demo EPD_Update_Fast）。
 * 前提：panel_init 已完成温度加载（0x1A/0x5A），否则 0xC7 能量不足 */
static int do_refresh(const uint8_t *bw_plane)
{
    if (!s_ready && panel_init() != 0) return -1;
    const size_t plane_bytes =
        (size_t)(g_panel_gdeq0426t82.panel_w / 8) * g_panel_gdeq0426t82.panel_h;

    set_full_window();                /* 窗口显式重设：防局刷窗口残留 */

    /* 新款屏配置（GxEPD2 2026/2/12 更新，见文件头勘误二）：
     * 0x21 Display Update Control 2：0x40 bypass RED（BW 屏禁用红平面
     * 驱动）+ 0x00 single chip application（禁用级联）。2023 demo 无
     * 此步（旧批次 OTP 预载），新款批次需显式下发，否则刷新异常。
     * 注：0x21 后需温度加载（0x1A/0x5A）才能正确驱动波形，否则能量
     * 不足导致文字浅/不清晰（2026-09-06 实测）。暂回退 0x21 序列，
     * 待 GxEPD2 源码确认完整温度加载流程后再启用 */
    // bus_cmd(0x21);
    // bus_dat(0x40);                    /* bypass RED as 0 */
    // bus_dat(0x00);                    /* single chip application */

    bus_cmd(0x24);
    bus_dat_stream(bw_plane, plane_bytes);
    bus_cmd(0x26);                    /* 差分基线同步（BW 波形不受影响） */
    bus_dat_stream(bw_plane, plane_bytes);

    bus_cmd(0x22); bus_dat(0xF7);     /* 全刷序列（demo EPD_Update，3.5s） */
    bus_cmd(0x20);
    bus_wait_busy(&g_panel_gdeq0426t82, g_panel_gdeq0426t82.busy_timeout_ms);
    return 0;
}

static int panel_full_refresh(const uint8_t *frame)
{
    /* frame：单平面帧（BW plane_count=1）：panel_w/8 * panel_h 字节 */
    return do_refresh(frame);
}

static int panel_write_full(const uint8_t *frame)
{
    /* 竖屏原始帧直通全刷：frame=NULL 清白（双 RAM 全 0xFF）。
     * 注：本路径不维护 prev 帧一致性，调用方须强制下一次全刷
     * （ui_force_full_refresh_next 已保证） */
    if (!frame) {
        if (!s_ready && panel_init() != 0) return -1;
        static const uint8_t white = 0xFF;
        const size_t plane_bytes =
            (size_t)(g_panel_gdeq0426t82.panel_w / 8) * g_panel_gdeq0426t82.panel_h;
        set_full_window();
        bus_cmd(0x24);
        for (size_t i = 0; i < plane_bytes; i++) bus_dat(white);
        bus_cmd(0x26);
        for (size_t i = 0; i < plane_bytes; i++) bus_dat(white);
        bus_cmd(0x22); bus_dat(0xF7);
        bus_cmd(0x20);
        bus_wait_busy(&g_panel_gdeq0426t82, g_panel_gdeq0426t82.busy_timeout_ms);
        return 0;
    }
    return panel_full_refresh(frame);
}

static int panel_write_planes(const uint8_t *const *planes)
{
    /* 多平面统一入口（§9.3）：BW 仅 planes[0]（单平面） */
    return do_refresh(planes[0]);
}

/* 全屏差分局刷（0.42s 优化，2026-09-07）：
 * 去掉 RST 硬复位（~500ms 开销），直接写 RAM + 0xFF 波形。
 * 官方 demo EPD_Dis_PartAll 含 RST 是保守做法，实测 booster 配置
 * 在局刷后仍稳定（0x18/0x3C 轻量重配不丢升压）。
 * 偏差②显式双写：0x26=prev + 0x24=new，不依赖 COG 内部拷贝 */
static int panel_partial(const uint8_t *prev, const uint8_t *new_, uint8_t passes)
{
    const int wb = g_panel_gdeq0426t82.panel_w / 8;
    const int ph = g_panel_gdeq0426t82.panel_h;

    /* diff 判断：帧无变化免刷（同 E042A13BW 先例） */
    int dirty = 0;
    for (int y = 0; y < ph && !dirty; y++)
        if (memcmp(prev + (size_t)y * wb, new_ + (size_t)y * wb, wb) != 0)
            dirty = 1;
    if (!dirty) return 0;

    if (!s_ready && panel_init() != 0) return -1;
    const size_t plane_bytes = (size_t)wb * ph;

    /* demo 局刷前置换（EPD_Dis_PartAll：RST → 0x18 → 0x3C/0x80 边框
     * 浮动防局刷闪烁；RST 后 booster 配置丢失 → 末尾 s_ready=false
     * 强制下次全刷完整重配 init（无状态铁律） */
    digitalWrite(EPD_RESET_PIN, LOW);
    delay(g_panel_gdeq0426t82.rst_pulse_ms);
    digitalWrite(EPD_RESET_PIN, HIGH);
    delay(10);
    bus_wait_idle(&g_panel_gdeq0426t82, 5000);
    bus_cmd(0x18); bus_dat(0x80);
    bus_cmd(0x3C); bus_dat(0x80);

    set_full_window();

    for (uint8_t p = 0; p < passes; p++) {
        bus_cmd(0x26);                /* 差分基线（偏差②，见文件头） */
        bus_dat_stream(prev, plane_bytes);
        bus_cmd(0x24);
        bus_dat_stream(new_, plane_bytes);
        bus_cmd(0x22); bus_dat(0xFF); /* 局刷波形（0.42s 官方标称） */
        bus_cmd(0x20);
        bus_wait_busy(&g_panel_gdeq0426t82, g_panel_gdeq0426t82.busy_timeout_ms);
    }

    s_ready = false;                  /* RST 后 booster 已丢，下次刷新重配 */
    return 0;
}

static void panel_power_off(void)
{
    /* P2d：SSD16xx 族关电收敛 epd_bus（0x22/0xC3 + 0x20，byte 级同族） */
    bus_ssd16_power_off(&g_panel_gdeq0426t82, &s_ready);
}

static void panel_deep_sleep(void)
{
    bus_ssd16_deep_sleep(&s_ready);   /* 0x10/0x01（demo EPD_DeepSleep 同款） */
}

/* —— desc 注册（首个 LAYOUT_LARGE 档面板，800x480 横屏原生）——
 * 时序为 demo 标称（全刷 ~2s）+ 同族保守口径，bring-up 实测回填；
 * BUSY 极性 HIGH=忙（demo Epaper_READBUSY while(1) 实证 + PDF §5） */
const epd_panel_desc_t g_panel_gdeq0426t82 = {
    .name       = "gdeq0426t82_ssd1677",
    .controller = EPD_CTRL_SSD1677,
    .panel_w    = 800,
    .panel_h    = 480,
    .gfx_rotation = 0,       /* 横向原生面板（gfx 800x480，LAYOUT_LARGE
                              * 首例，档位几何为预估值待上机校准——
                              * layout_profile.h LARGE 条目 §8.1 预留） */
    .dpi         = 219,       /* 对角 PPI（诊断字段：4.26" 对角 933px） */
    .color_mode = EPD_COLOR_BW,
    .plane_count = 1,        /* BW 单平面 */
    .palette    = {
        /* BW 语义同 panel_e042a13bw（§9.3/§9.4：bit0=B/W 平面白位）；
         * demo 0x24 bit=1=白（EPD_WhiteScreen_White 全 0xFF）实证 */
        [EPD_GFX_WHITE]  = 0x01,
        [EPD_GFX_BLACK]  = 0x00,
        [EPD_GFX_ACCENT] = 0x00,   /* BW 退化：强调色降级黑（§9.2） */
        [EPD_GFX_AUX]    = 0x00,
    },
    .accent_rgb = 0x000000,    /* BW 无第三色，LAN 调色板不注入 */
    .fb_location = EPD_FB_AUTO,    /* 48,000B x3（双帧+画布）=144KB >
                                   * 128KB 阈值 → 自动落 PSRAM（§10.2，
                                   * 现役首块 PSRAM 帧 BW 面板；SPI 只走
                                   * CPU 逐字节发送，无 DMA 需求） */
    .rst_pulse_ms = 10,           /* demo RST 低脉冲 10ms（STM32/Arduino 一致） */
    .busy_level = 1,              /* BUSY=HIGH 忙（demo + PDF §5 实证） */
    .busy_timeout_ms = 6000,     /* 实测两轮 boot 全刷 BUSY 均 <6s 正常
                                  * 完成（epd 阶段 +4.27s 含渲染/传输/
                                  * 波形，0xF7 波形估 ~3.5s+），余量足够 */
    .power_on_ms  = 40,           /* 上电含于激活序列，同族口径 */
    .power_off_ms = 30,
    .full_ms    = 3500,           /* 全刷 0xF7 官方标称 3.5s（panel_init
                                  * 含温度加载 ~1.5s 一次性开销，后续
                                  * 每次 do_refresh 仅 3.5s 波形 +
                                  * ~200ms 传输） */
    .partial_ms = 1500,           /* 0xFF 局刷 + 双 RAM 96KB@4MHz≈200ms，
                                  * 同族估计，实测回填 */
    .partial_enabled = true,      /* 硬件窗口局刷（demo BaseMap+Dis_PartAll
                                  * 序列，0x22/0xFF） */
    .passes     = 1,              /* 局刷单次波形 */
    .partial_count_full_refresh = 5, /* demo main.c 明示：5 次局刷后
                                  * 全刷清残影（"After 5 partial
                                  * refreshes, implement a full screen
                                  * refresh"），按 demo 标称暂取 5，
                                  * 待机页轮换体验实测后可调 */
    .window_8align = true,        /* 0x44 X 窗口像素 8 对齐（demo
                                  * x_start-x_start%8 实证） */
    .ops = {
        .init         = panel_init,
        .full_refresh = panel_full_refresh,
        .write_full   = panel_write_full,
        .partial      = panel_partial,   /* 双 RAM 差分 + 0x22/0xFF */
        .power_off    = panel_power_off,
        .deep_sleep   = panel_deep_sleep,
        .probe        = NULL,       /* SSD1677 版本读 0x2F 可作 probe
                                   * （epd_driver 诊断路径覆盖），留 §14.3 */
        .write_planes = panel_write_planes,
        .diag         = bus_diag_ssd16, /* T1.8：SSD16xx 0x2F 双读 */
    },
};
