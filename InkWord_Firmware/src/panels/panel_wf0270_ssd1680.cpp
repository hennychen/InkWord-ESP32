/**
 * @file panel_wf0270_ssd1680.cpp
 * @brief WEIFENG WF0270 面板单元（L0）—— SSD1680 手写序列 + 三色 desc
 *        （PANEL_COMPAT_DESIGN §十六 SOP 第三块屏 / LAYOUT_SMALL 首例）
 *
 * 面板：维峰 WEIFENG WF0270T1PCZ2200E4，2.7" 264x176 黑白红三色，
 * 22Pin 0.5mm FPC，3.3V。IC 推断 SSD1680（高置信度：同族 Solomon
 * SSD16xx，与 Waveshare 2.7" e-Paper (B) V2 同规格同分辨率；待规格书
 * 确认，bring-up 时 0x2F 版本读 + 序列响应度实锤）。
 *
 * 几何（demo SetWindows 实证，2026-08-22 核对）：COG 物理竖屏 ——
 * 源极（X，0x44 字节窗口）176px / 门极（Y，0x45 窗口）264 线，
 * panel_w=176 / panel_h=264，gfx_rotation=1 → UI 横屏 264x176
 * （同 DEPG0370 先例；layout_profile LAYOUT_SMALL 档位 §8.1 预留）。
 * !! 方案初稿 panel_w=264/panel_h=176 为笔误——源极窗口超 176 会
 * 使 0x44/0x45 与 RAM 布局转置错位（SSD1680 源极上限 176px）。
 *
 * 序列来源（权威，一比一移植）：Waveshare 2.7inch e-Paper (B) V2
 * 官方 demo epd2in7b_V2.cpp（同为 SSD1680 264x176 BWR）——Init 极简
 * （无 0x74/0x7E/0x0C/0x2B 软启动链，OTP 预载批次）、刷新直接 0x20
 * Master Activation（PSR 0x27 已含 LUT/色序配置，无需 SSD1619 的
 * 0x22/0xF7 显式序列）。与 E042A13（SSD1619，2017 批次须显式初始
 * 代码）差异见各函数注释。
 *
 * 双 RAM 语义（SSD16xx 家族一致）：0x24 B/W RAM bit=1 白 + 0x26
 * color RAM bit=1 红。demo DisplayFrame 黑平面 ~buffer 取反系其
 * image 约定（bit=0=白），本单元 plane[0] 约定 bit=1=白直通，
 * 不移植取反（ClearFrame 白=0xFF/红=0x00 证实 0x24 bit=1=白）。
 *
 * 无状态设计（desc 头注释铁律，同 E042A13）：do_refresh 前置
 * s_ready 检查，power_off/deep_sleep 后归零，下次刷新自动 RST
 * 唤醒 + 完整重配。
 */
#include "../epd_panel.h"
#include "../gpio_config.h"
#include "epd_bus.h"    /* T1.1：SPI 原语/等待收敛层 */

#include <Arduino.h>
#include <SPI.h>

/* 前置声明：ops 实现引用 desc 几何/时序字段 */
extern const epd_panel_desc_t g_panel_wf0270;

static bool s_ready = false;   /* init 完成（power_off/deep_sleep 归零） */


static int panel_init(void)
{
    /* Waveshare epd2in7b_V2 Init 一比一（2021 量产批次 OTP 预载，
     * 与 E042A13 的 GDEH042Z96 §4.1 显式初始代码路径不同——本屏
     * Init 仅 PSR/数据入口/窗口/光标四步，多余初始代码反而可能
     * 覆盖 OTP 标定值） */
    pinMode(EPD_RESET_PIN, OUTPUT);
    pinMode(EPD_CS_PIN, OUTPUT);
    pinMode(EPD_DC_PIN, OUTPUT);
    pinMode(EPD_BUSY_PIN, INPUT);
    digitalWrite(EPD_CS_PIN, HIGH);
    digitalWrite(EPD_DC_PIN, LOW);

    /* demo Reset()：HIGH 200ms → LOW rst_pulse_ms → HIGH 200ms
     * （保持远长于 E042A13 的 10/20/10，VCI 升压充分再 boot） */
    digitalWrite(EPD_RESET_PIN, HIGH);
    delay(200);
    digitalWrite(EPD_RESET_PIN, LOW);
    delay(g_panel_wf0270.rst_pulse_ms);
    digitalWrite(EPD_RESET_PIN, HIGH);
    delay(200);
    bus_wait_idle(&g_panel_wf0270, 5000);            /* 复位后 boot 自检（忙→闲） */

    bus_cmd(0x12);                    /* SWRESET：寄存器回 POR，VCOM 载 OTP */
    bus_wait_idle(&g_panel_wf0270, 5000);

    /* —— PSR（demo 原样 3 字节：0x27 主配置，后两字节 demo 冗余下发，
     * COG 按命令长度截断，无害保留）—— 0x27 = REG LUT + BWR 色序，
     * LUT 取 OTP 波形表（0x20 直接激活即跑全刷序列） —— */
    bus_cmd(0x00);
    bus_dat(0x27);
    bus_dat(0x01);
    bus_dat(0x00);

    bus_cmd(0x11); bus_dat(0x03);     /* 数据入口 X+ Y+ */

    /* RAM 窗口：X 字节 0..panel_w/8-1（176/8=22 → 0..21），
     * Y 0..panel_h-1（264 → 0..0x0107，demo SetWindows 同款） */
    bus_cmd(0x44);
    bus_dat(0x00);
    bus_dat((uint8_t)(g_panel_wf0270.panel_w / 8 - 1));
    bus_cmd(0x45);
    bus_dat(0x00); bus_dat(0x00);
    bus_dat((uint8_t)((g_panel_wf0270.panel_h - 1) & 0xFF));
    bus_dat((uint8_t)((g_panel_wf0270.panel_h - 1) >> 8));

    bus_cmd(0x4E); bus_dat(0x00);     /* X 计数器归零 */
    bus_cmd(0x4F); bus_dat(0x00); bus_dat(0x00);   /* Y 计数器归零 */
    bus_wait_idle(&g_panel_wf0270, 5000);

    s_ready = true;
    return 0;
}

/* 双平面写入 + 真全刷（demo DisplayFrame 一比一）：
 * planes[0] = B/W 平面（bit=1 白）→ 0x24；
 * planes[1] = 红位平面（bit=1 红）→ 0x26（直通，见文件头语义注）；
 * 刷新直接 0x20 Master Activation——SSD1680 PSR(0x27) 下内部自动
 * 跑上电/温度/LUT/显示全序列（demo DisplayFrame() 无 0x22，
 * 与 SSD1619 的 0x22/0xF7 显式序列不同） */
static int do_refresh(const uint8_t *bw_plane, const uint8_t *red_plane)
{
    if (!s_ready && panel_init() != 0) return -1;
    const size_t plane_bytes =
        (size_t)(g_panel_wf0270.panel_w / 8) * g_panel_wf0270.panel_h;

    /* 地址计数器归零（无状态保证：不依赖上次写满后的回卷状态） */
    bus_cmd(0x4E); bus_dat(0x00);
    bus_cmd(0x4F); bus_dat(0x00); bus_dat(0x00);

    bus_cmd(0x24);
    bus_dat_stream(bw_plane, plane_bytes);
    bus_cmd(0x26);
    bus_dat_stream(red_plane, plane_bytes);

    bus_cmd(0x20);                    /* Master Activation（PSR 全配置） */
    bus_wait_busy(&g_panel_wf0270, g_panel_wf0270.busy_timeout_ms);              /* 三色波形 ~15s：等真实完成再下电 */
    return 0;
}

static int panel_full_refresh(const uint8_t *frame)
{
    /* frame：双平面连续布局（Phase 2 约定）：plane_count x panel_w/8
     * x panel_h 字节，[0]=B/W 白位平面、[1]=红位平面 */
    const size_t plane_bytes =
        (size_t)(g_panel_wf0270.panel_w / 8) * g_panel_wf0270.panel_h;
    return do_refresh(frame, frame + plane_bytes);
}

static int panel_write_full(const uint8_t *frame)
{
    /* 竖屏原始帧直通全刷（epd_full_refresh / LAN 语义）：frame=NULL
     * 清白（demo ClearFrame：黑 RAM 全 0xFF + 红 RAM 全 0x00 + 0x20）。
     * 注：本路径不维护 prev 帧一致性，调用方须强制下一次全刷
     * （ui_force_full_refresh_next 已保证） */
    if (!frame) {
        if (!s_ready && panel_init() != 0) return -1;
        static const uint8_t white = 0xFF, no_red = 0x00;
        const size_t plane_bytes =
            (size_t)(g_panel_wf0270.panel_w / 8) * g_panel_wf0270.panel_h;
        bus_cmd(0x4E); bus_dat(0x00);
        bus_cmd(0x4F); bus_dat(0x00); bus_dat(0x00);
        bus_cmd(0x24);
        for (size_t i = 0; i < plane_bytes; i++) bus_dat(white);
        bus_cmd(0x26);
        for (size_t i = 0; i < plane_bytes; i++) bus_dat(no_red);
        bus_cmd(0x20);
        bus_wait_busy(&g_panel_wf0270, g_panel_wf0270.busy_timeout_ms);
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
    /* P2d：SSD16xx 族关电收敛 epd_bus（三家面板 byte 级一致） */
    bus_ssd16_power_off(&g_panel_wf0270, &s_ready);
}

static void panel_deep_sleep(void)
{
    bus_ssd16_deep_sleep(&s_ready);
}

/* —— desc 注册（第三面板单元，LAYOUT_SMALL 档首例）——
 * 时序：同族估计口径（SSD1680 2.7" 三色全刷 ~15s），bring-up 后
 * 按实测回填；BUSY 极性 HIGH=忙（demo WaitUntilIdle while(busy==1)
 * 实证）；plane 语义 0x24 bit=1 白 / 0x26 bit=1 红（demo ClearFrame
 * 反证） */
const epd_panel_desc_t g_panel_wf0270 = {
    .name       = "wf0270_ssd1680",
    .controller = EPD_CTRL_SSD1680,
    .panel_w    = 176,
    .panel_h    = 264,
    .gfx_rotation = 1,        /* 物理竖屏 176x264 → UI 横屏 264x176
                               * （顺时针 90°，同 DEPG0370 先例；
                               * LAYOUT_SMALL 档 264x176 §8.1 预留） */
    .dpi         = 118,       /* 对角 PPI（诊断字段：2.7" 对角 317px；换算 mm=px÷PPI×25.4） */
    .color_mode = EPD_COLOR_3C,
    .plane_count = 2,         /* B/W 白位平面 + 红位平面（§9.3） */
    .palette    = {
        /* 逻辑色 → 平面置位掩码：bit0=B/W 平面白位，bit1=红平面红位
         * （三色映射与 E042A13 一致，SSD16xx 家族 RAM 语义同源） */
        [EPD_GFX_WHITE]  = 0x01,   /* (bw=1,red=0) → 白 */
        [EPD_GFX_BLACK]  = 0x00,   /* (0,0) → 黑 */
        [EPD_GFX_ACCENT] = 0x02,   /* (0,1) → 红 */
        [EPD_GFX_AUX]    = 0x00,   /* 3C 无第四色 → 退化黑（§9.2） */
    },
    .accent_rgb = 0xFF0000,    /* 红（LAN 量化调色板注入） */
    .fb_location = EPD_FB_AUTO,   /* 双帧+画布 ~34.8KB << 128KB → 全 SRAM
                                   * （5,808B x2 平面 x2 帧 x~1.5；与
                                   * WiFi/BLE 共存余量最宽裕） */
    .rst_pulse_ms = 2,            /* demo Reset() LOW 宽度（前后 HIGH
                                   * 各 200ms 在 panel_init 内） */
    .busy_level = 1,              /* BUSY=HIGH 忙（SSD16xx 系，demo
                                   * while(busy==1) 实证） */
    .busy_timeout_ms = 20000,     /* 同族保守值，实测后回填 */
    .power_on_ms  = 40,           /* 上电含于 0x20 激活序列，近似值 */
    .power_off_ms = 30,
    .full_ms    = 15000,          /* SSD1680 2.7" 三色同族 ~15s 估计，
                                   * bring-up 实测后回填 */
    .partial_ms = 16000,          /* 无快速局刷：partial==full */
    .partial_enabled = false,     /* 三色面板一律 false（§13.2 厂商级确认） */
    .passes     = 1,
    .partial_count_full_refresh = 1, /* 无局刷：阈值不参与调度，保守 1 */
    .window_8align = true,        /* 0x44 窗口 x 8 像素对齐（176/8=22 ✓） */
    .ops = {
        .init         = panel_init,
        .full_refresh = panel_full_refresh,
        .write_full   = panel_write_full,
        .partial      = NULL,     /* partial_enabled=false，L3 不触达 */
        .power_off    = panel_power_off,
        .deep_sleep   = panel_deep_sleep,
        .probe        = NULL,     /* SSD1680 0x2F 版本读可作 probe
                                   * （epd_driver 诊断路径已覆盖），
                                   * 留 §14.3 接入 */
        .write_planes = panel_write_planes,
        .diag         = bus_diag_ssd16, /* T1.8：SSD16xx 0x2F 双读 */
    },
};
