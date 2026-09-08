/**
 * @file epd_driver.cpp
 * @brief 墨水屏 GFX 驱动 — 多面板双画布架构（L3）
 *
 * 硬件：ESP32-S3 + 转接板（板级轴 INKWORD_BOARD_*，默认 EVK011，
 *       可切 v1.4 通用板，见 gpio_config.h）+ 面板轴（构建矩阵
 *       EPD_PANEL_DEFAULT_ID，默认 DEPG0370 3.7" BW / env 钉面板，
 *       见 epd_panel.h）
 *
 * 架构（2026-08-18 残影叠加修复后确定；2026-08-22 Phase 1 面板序列迁入
 *       panels/，本层经 L2 desc.ops 调用；Phase 2 帧缓冲/画布/转置几何
 *       全部运行期取自 desc，魔数清零；Phase 3+6 双画布色彩路径：
 *       逻辑色轻路由 + 多平面展开，三色面板首次可用）：
 *   - UI 绘图画布：双层 GFXcanvas1（§9.4）——B/W 层（bit=1 白）+
 *     强调色层（bit=1 红，仅多平面面板分配）；逻辑色在 epd_gfx_*
 *     入口轻路由分解到两层，Adafruit GFX 完整字体栈保留，
 *     getBuffer() 公开可读 —— GxEPD2_BW 的 _buffer 为 private 无法取旧帧，
 *     这是放弃其 displayWindow 增量路径的直接原因）
 *   - 帧缓冲：自持双帧（s_port_new/s_port_prev，多平面连续布局），
 *     全刷 = writeImageForFullRefresh(双写 0x10+0x13)+refresh(false)；
 *     局刷 = demo 忠实序列：硬复位 → partial 初始化 → 双 RAM 写窗口
 *     （旧帧→0x10 差分基准，新帧→0x13）→ 0x04/0x12/0x02，
 *     完全无状态，不依赖 COG 内部 RAM 跨刷新存活
 *     （对照 Info/ 官方 demo Display_windows_image_partial_update；
 *      GxEPD2 增量路径只写 0x13、依赖 COG 0x10 持久 —— 本面板上不可靠，
 *      残影叠加根因，真机连续翻词 20+ 次字迹叠加实测）
 *   - 升压（两板均无 MCU 信号职责）：EVK011 板上分立 boost 由屏幕
 *     COG 从 FPC pin2(GDR) 自主驱动；v1.4 板载自主升压（解耦 COG 时
 *     序）—— 均不输出任何 GDR/RESE 信号，仅供 VCI 3.3V
 *   - 信号：BS1=LOW(4线SPI)，BUSY=LOW 忙，全刷 CDI=0x97，局刷 CDI=0x17
 *
 * 本文件提供 C API（epd_gfx_* 系列供 .c 模块使用），
 * 文字渲染使用 FreeSans 矢量字体（setCursor 的 y 为文本基线）。
 */

#include "epd_driver.h"
#include "gpio_config.h"
#include "epd_panel.h"
#include "epd_geom.h"   /* T2.1：转置/窗口/调色板纯函数权威（native 真值表） */
#include "layout_profile.h"  /* set_dpi：PPI 自动层注入（2026-09-08，
                              * 首调 get 前时序保证=init 内早于 UI） */

#include <Arduino.h>
#include <SPI.h>
#include "debug_log.h"   /* 必须在 Arduino.h 之后：还原被 esp32-hal-log 劫持的 ESP_LOGx */
#include <Adafruit_GFX.h>

#include <Fonts/FreeSans9pt7b.h>
#include "Fonts/Arial14pt7b.h"   /* 官方 fontconvert 从 Arial.ttf 生成（GFX 库无 14pt 档） */
#include <Fonts/FreeSans18pt7b.h>
#include <Fonts/FreeSans24pt7b.h>
/* Bold 表（2026-08-27 P2 设置「粗细」）：GFX 库无 14pt 档（Arial14pt7b
 * 需本地生成同理），14pt 槽降用 FreeSansBold12pt7b——保住四档粗细均
 * 可切换，代价是 14pt 档加粗时字号降 12pt（状态栏/菜单，真机目检）；
 * 后续可用官方 fontconvert 生成 FreeSansBold14pt7b 补齐 */
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

#include <string.h>
#include <stdlib.h>     /* malloc：帧缓冲内部 SRAM 分配 */
#include <esp_heap_caps.h> /* Phase 2：PSRAM 帧缓冲 heap_caps_malloc（禁 DMA cap） */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h" /* T0.1：帧一致序列互斥锁 */
#include "nvs.h"         /* P1 运行期选屏：set_panel 覆盖读 */
#include "settings_keys.h"   /* P2b：NVS 键权威表 */

/* L2 面板描述符（Phase 1）：epd_panel_get_by_id 查表所得，全局唯一；
 * 面板类实例与 demo 时序序列封装在 panels/panel_depg0370_uc8253.cpp，
 * 本层只经 s_panel->ops 调用，不触碰 GxEPD2 面板类（铁律 3） */
static const epd_panel_desc_t *s_panel = NULL;

/* 生效旋转（2026-08-26 屏幕方向设置）：desc.gfx_rotation 为面板出厂
 * 默认 UI 方向（const 注册表不变），本变量为运行期生效值——init 取
 * desc 默认，epd_set_rotation 运行期覆盖。转置/窗口映射/画布几何
 * 三处消费；帧缓冲按面板物理几何分配，与旋转无关不重分配 */
static uint8_t s_rot = 0;

/* UI 绘图画布（双层，§9.4）：B/W 层 bit=1 白，堆分配，getBuffer() 公开可读；
 * s_canvas_ac 强调色层 bit=1 红，仅 plane_count>1 面板分配（BW 面板
 * NULL，绘图路由自动退化为单层，行为与既有单画布时代完全一致） */
static GFXcanvas1 *s_canvas = NULL;
static GFXcanvas1 *s_canvas_ac = NULL;

/* 自持双帧（竖屏，行宽 panel_w/8 字节，bit=1 白，与 COG SRAM 语义一致）：
 * s_port_new  = 最近一次绘制转置结果（待刷新/已刷新的新帧）
 * s_port_prev = 屏幕当前真实内容快照（局刷 0x10 差分基准）
 * Phase 2 动态化：按 desc 几何/平面数在 epd_driver_init 内堆分配
 * （多平面连续布局，§6.2），AUTO 阈值规则见 epd_fb_alloc */
static uint8_t *s_port_new  = NULL;
static uint8_t *s_port_prev = NULL;
static size_t   s_fb_size   = 0;      /* 单平面单帧 = panel_stride x panel_h */
static bool     s_fb_in_psram = false; /* 诊断日志：帧缓冲实际落点 */

static bool s_inited = false;

/* 帧一致序列互斥锁（T0.1，修 C1）：httpd 任务（LAN 直刷）与按键任务
 * （渲染局刷）对 SPI 总线 / s_port_prev / 画布的并发访问防护——
 * lan_display_server.cpp 文件头自认的已知竞态。绘图 API（fill_rect /
 * draw_text 等）不加锁：单写者场景下绘制与刷新由上层序列保证；锁只
 * 保护「转置 → ops 写帧 → BUSY 等待 → prev 回写」的帧一致序列，
 * 避免锁粒度膨胀。超时放弃本次刷新：屏幕保持旧帧好于错帧 */
static SemaphoreHandle_t s_epd_lock = NULL;
#define EPD_LOCK_TIMEOUT_MS 15000  /* 覆盖最长全刷 14.6s（三色）+ 余量 */

static bool epd_lock(void)
{
    /* 未创建（init 失败/早期路径）时无并发方，直通放行 */
    if (!s_epd_lock) return true;
    return xSemaphoreTake(s_epd_lock, pdMS_TO_TICKS(EPD_LOCK_TIMEOUT_MS)) == pdTRUE;
}

static void epd_unlock(void)
{
    if (s_epd_lock) xSemaphoreGive(s_epd_lock);
}

/* 面板竖屏行宽字节（字节粒度向上取整）：OPM021EB 122px → 16B/行
 * 为首个非 8 整除宽面板（2026-08-30），SSD1680 RAM 物理行宽同 16B
 * （122px 有效 + 6 位无源极线不显示）；8 整除面板 (w+7)/8 == w/8，
 * 全部既有面板零行为差异 */
static inline int panel_stride(const epd_panel_desc_t *p)
    { return (p->panel_w + 7) / 8; }

static const char *TAG = "EPD"; /* debug_log 宏依赖 */

/* font_size: 1=小(9pt) 2=中(14pt,默认) 3=大(18pt) 4=特大(24pt)
 * 注：14pt 档为 Arial14pt7b（Helvetica 风格，与 FreeSans 视觉一致） */
static const GFXfont *s_fonts[] = {
    &FreeSans9pt7b,
    &Arial14pt7b,
    &FreeSans18pt7b,
    &FreeSans24pt7b,
};
/* Bold 表（2026-08-27 P2 设置「粗细」）：epd_gfx_set_bold 注入（main
 * 初始化 NVS 恢复 / settings 切换即时 apply，音量→es8311 同范式）；
 * 仅 FreeSans/ASCII 路径生效，CJK 点阵（cjk_text）不受影响。
 * 14pt 槽降用 12pt Bold（库无 14pt 档，见上方 include 注） */
static const GFXfont *s_fonts_bold[] = {
    &FreeSansBold9pt7b,
    &FreeSansBold12pt7b,
    &FreeSansBold18pt7b,
    &FreeSansBold24pt7b,
};
static bool s_bold = false;      /* 默认关：视觉零变化铁律 */

void epd_gfx_set_bold(bool on)
{
    s_bold = on;
}

static const GFXfont *font_for_size(int font_size)
{
    if (font_size < 1 || font_size > 4) font_size = 2;
    return (s_bold ? s_fonts_bold : s_fonts)[font_size - 1];
}

/* GFXcanvas1 1bpp 画布色（数值与 GxEPD2 的 GxEPD_BLACK/GxEPD_WHITE 一致：
 * 0x0000 → bit=0 黑 / 0xFFFF → bit=1 白，画布 bit=1 白，见 canvas_to_panel
 * 注释；Phase 1 解耦 GxEPD2 头文件后本地等值定义） */
static const uint16_t CANVAS_BLACK = 0x0000;
static const uint16_t CANVAS_WHITE = 0xFFFF;

/* 逻辑色 → 双层画布色分解（Phase 6，§9.4；T2.1 后公式权威在
 * epd_geom，本处仅转发）：
 *   B/W 层：WHITE 置白位，BLACK/ACCENT 置黑位（红像素需 B/W 位为黑，
 *           IL0398 真值表 (0,1)=红）；
 *   AC 层：ACCENT 置红位，其余清位（BLACK 绘制必须清红防旧红残留）；
 * BW 单平面面板无 AC 层，仅 B/W 层参与，与旧单画布行为等价 */
static uint16_t bw_layer_color(uint16_t color)
{
    return epd_geom_bw_layer_color(color);
}

static uint16_t ac_layer_color(uint16_t color)
{
    /* 2026-08-22 真机勘误：初版条件反了（ACCENT→清位、其余→置位），
     * 导致 fill_screen(WHITE) 后 AC 层全 1 → red plane 全置 → 整屏恒红
     * （E042A13 bring-up 实测：波形照跑但待机页永不显现）。AC 层
     * CANVAS_WHITE(0xFFFF) 语义＝位全 1＝红，非「白」——命名易误导 */
    return epd_geom_ac_layer_color(color);
}

/* 帧缓冲分配（Phase 2，§6.2/§10.2）：来源按 desc.fb_location ——
 * AUTO 以「双帧+画布」合计 128KB 为界（S3 内部 SRAM 扣 WiFi/BLE/lwIP
 * 栈后约 250-300KB 可用，留足余量），超限落 PSRAM；显式 SRAM/PSRAM
 * 则直配。帧缓冲只走 CPU 读写 + SPI 逐行发送，禁用 MALLOC_CAP_DMA
 * （PSRAM DMA 误配是已知陷阱）。分配失败返回 NULL 由调用方报错 */
static uint8_t *epd_fb_alloc(size_t size)
{
    const size_t total = s_fb_size * s_panel->plane_count * 3; /* 双帧+画布 */
    const bool psram = (s_panel->fb_location == EPD_FB_PSRAM) ||
                       (s_panel->fb_location == EPD_FB_AUTO && total > 128u * 1024u);
    uint8_t *p = psram ? (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM)
                       : (uint8_t *)malloc(size);
    s_fb_in_psram = psram; /* 两帧同池分配，取末次判定即可 */
    return p;
}

/* 单画布层 → 指定面板平面转置（Phase 2 四方向泛化提取，§6.3；对应
 * GxEPD2_BW setRotation 语义，_reverse=false，现役 rot=1 顺时针 90°）。
 * 画布与平面同为 bit=1 置位（B/W 层 bit=1 白 / AC 层 bit=1 红），
 * 置位直通，无需反相。T2.1 后公式权威在 epd_geom（native 真值表） */
static void transpose_to_plane(const GFXcanvas1 *cv, uint8_t *plane)
{
    epd_geom_transpose(cv->getBuffer(), cv->width(), cv->height(),
                       plane, panel_stride(s_panel), (int)s_rot);
}

/* 双层画布 → 面板多平面展开（Phase 6，§9.3）：
 * plane[0] = B/W 白位平面（bit=1 白）、plane[1] = 红位平面（bit=1 红），
 * 多平面连续布局与 Phase 2 帧缓冲约定一致（ops.full_refresh 直通）；
 * 单平面面板仅写 plane[0]，行为与 Phase 2 完全一致 */
static void canvas_to_panel(uint8_t *panel)
{
    memset(panel, 0x00, s_fb_size * s_panel->plane_count);
    transpose_to_plane(s_canvas, panel);
    if (s_canvas_ac && s_panel->plane_count > 1)
        transpose_to_plane(s_canvas_ac, panel + s_fb_size);
}

/* GFX 窗口 → 面板窗口（Phase 2 四方向泛化，§6.3；rot=1 即旧
 * swap(x,y)/swap(w,h)+翻转，与 GxEPD2_BW displayWindow 的
 * _rotate+._reverse 语义一致）。T2.1 后公式权威在 epd_geom。
 * 注：面板侧 x/w 由 epd2 层自动 8 像素对齐（对应 gfx 侧 y/h 对齐约束不变） */
static void gfx_rect_to_panel(int x, int y, int w, int h,
                              uint16_t *px, uint16_t *py, uint16_t *pw, uint16_t *ph)
{
    epd_geom_rect_to_panel(x, y, w, h, s_canvas->width(), s_canvas->height(),
                           (int)s_rot, px, py, pw, ph);
}

/* ============================================================
 * 公共 C API 实现
 * ============================================================ */

extern "C" {

int epd_driver_init(void)
{
    if (s_inited) {
        LOG_W("epd_driver already initialized, skip");
        return 0;
    }

    /* 0. L2 面板描述符查表（Phase 1 唯一面板；Phase 3 起构建矩阵注入）。
     *    P1 收官（2026-09-02）NVS 运行期选屏："inkword"/set_panel 存
     *    面板注册名（字符串主键，注册表追加不漂移；键缺失=跟随构建
     *    默认），一固件任意换屏（产线/售后同包烧录）。错选型号屏不亮
     *    时的无屏恢复路径：按住 RST 侧键上电直读 GPIO（无源开关按下
     *    接地，gpio_config.h 按键区；button_handler 尚未初始化，绕过
     *    去抖/队列直接读电平），本次启动忽略 NVS 覆盖回落构建默认，
     *    亮屏后回设置页「面板型号」改回「默认」 */
    const char *panel_id = EPD_PANEL_DEFAULT_ID;
    char nvs_id[32];
    pinMode(NAV_RST_PIN, INPUT_PULLUP);
    if (digitalRead(NAV_RST_PIN) == LOW) {
        LOG_W("NAV_RST held at boot: skip NVS panel override");
    } else {
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            size_t len = sizeof(nvs_id);
            if (nvs_get_str(h, NVS_KEY_SET_PANEL, nvs_id, &len) == ESP_OK)
                panel_id = nvs_id;
            nvs_close(h);
        }
    }
    s_panel = epd_panel_get_by_id(panel_id);
    if (!s_panel && strcmp(panel_id, EPD_PANEL_DEFAULT_ID) != 0) {
        /* NVS 值未命中注册表（型号拼写漂移/裁剪残留）：回落构建默认，
         * 不进下方拒绝路径——那是 DEFAULT_ID 也查不到的构建期错误专属 */
        LOG_W("NVS set_panel '%s' not in registry, fallback '%s'",
              panel_id, EPD_PANEL_DEFAULT_ID);
        panel_id = EPD_PANEL_DEFAULT_ID;
        s_panel = epd_panel_get_by_id(panel_id);
    }
    if (!s_panel) {
        LOG_E("panel desc '%s' not found in registry", panel_id);
        return -1;
    }
    if (strcmp(panel_id, EPD_PANEL_DEFAULT_ID) != 0)
        LOG_I("panel '%s' selected (NVS override, build default '%s')",
              panel_id, EPD_PANEL_DEFAULT_ID);

    /* 0.2 P3 desc 契约校验（2026-09-05）：全注册表开机过一遍（违规
     *     LOG_W——未选中屏也暴露，新屏 desc 笔误首次任意屏启动即
     *     现形，不必换屏烧录才能发现）；另查注册名唯一性（strcmp
     *     主键查表前提）。选中面板违规 fail-fast（desc 违规属构建
     *     期错误，拒绝带病初始化）。纯字段校验 ~10 屏微秒级 */
    {
        char verr[80];
        for (int i = 0; i < epd_panel_registry_count(); i++) {
            const epd_panel_desc_t *rd = epd_panel_at(i);
            if (epd_panel_desc_check(rd, verr, sizeof(verr)) != 0)
                LOG_W("registry[%d] '%s' desc contract: %s",
                      i, rd->name, verr);
            for (int j = i + 1; j < epd_panel_registry_count(); j++)
                if (strcmp(epd_panel_at(j)->name, rd->name) == 0)
                    LOG_W("registry[%d]/[%d] duplicate name '%s'",
                          i, j, rd->name);
        }
        if (epd_panel_desc_check(s_panel, verr, sizeof(verr)) != 0) {
            LOG_E("selected panel '%s' desc contract: %s", panel_id, verr);
            return -1;
        }
    }

    /* 0.5 帧一致序列互斥锁（T0.1）：先于首次刷新创建（epd_clear_screen
     * 在 init 返回后即被 main 调用）；此后所有 ops 刷新序列均持锁 */
    if (!s_epd_lock) s_epd_lock = xSemaphoreCreateMutex();

    /* 1. BS1=LOW 选择 4 线 SPI 模式（EVK011 J2-10；v1.4 板上硬接无此步）。
     *    EVK011 省线方案：在转接板侧将 J2-10 直接短接 GND（板上就近接
     *    J2-1），并把 gpio_config.h 的 EPD_BS_PIN 改为 -1 —— 硬接 GND
     *    比 GPIO 驱动更稳（ESP32 启动前 ~100ms 该脚高阻，硬接 GND
     *    无采样不定窗口）；v1.4 已在板上固化此优势 */
#if EPD_BS_PIN >= 0
    pinMode(EPD_BS_PIN, OUTPUT);
    digitalWrite(EPD_BS_PIN, LOW);
#endif

#if INKWORD_EPD_DIAG
    /* 2. BUSY 三态诊断：
     *    a) 高阻输入读电平：COG 空闲时应为 1
     *    b) 开内部上拉(≈45kΩ)再读：区分「悬空断线」与「被驱动为低」
     *       - 上拉后变 1 → 线悬空：BUSY 断线/接错针/FPC 未连通
     *       - 上拉后仍 0 → 有源驱动低：COG 在拉低（或线对地短路） */
    pinMode(EPD_BUSY_PIN, INPUT);
    Serial.printf("[EPD-DIAG] pins: BS=%d SCK=%d MOSI=%d DC=%d CS=%d RST=%d BUSY=%d\n",
                  EPD_BS_PIN, EPD_SCK_PIN, EPD_MOSI_PIN, EPD_DC_PIN,
                  EPD_CS_PIN, EPD_RESET_PIN, EPD_BUSY_PIN);
    int b_float = digitalRead(EPD_BUSY_PIN);
    pinMode(EPD_BUSY_PIN, INPUT_PULLUP);
    delay(5);
    int b_pulled = digitalRead(EPD_BUSY_PIN);
    pinMode(EPD_BUSY_PIN, INPUT); /* 恢复高阻，交回驱动 */
    const char *busy_verdict = (b_float == 1) ? "HIGH (idle — or floating, see RST test below)"
                               : (b_pulled == 1) ? "FLOATING (open wire / wrong pin / FPC not seated)"
                               : "DRIVEN LOW (COG busy, or short to GND)";
    Serial.printf("[EPD-DIAG] BUSY hi-Z: %d | with-pullup: %d -> %s\n",
                  b_float, b_pulled, busy_verdict);

    /* 2b. RST 复位脉冲测试（决定性，区分「COG 真活着」与「BUSY 悬空浮高」）：
     *     COG 复位后自检会进入忙态（极性取 desc.busy_level，面板轴泛化：
     *     UC8253 拉低 / SSD16xx 拉高）。
     *     出现忙电平 → COG 供电+GND+BUSY 线+RST 线全通
     *     无反应     → 屏断电(VCI/GND)/FPC 未插/BUSY 线断/RST 线断 */
    pinMode(EPD_RESET_PIN, OUTPUT);
    digitalWrite(EPD_RESET_PIN, HIGH);
    delay(10);
    digitalWrite(EPD_RESET_PIN, LOW);
    delay(20);                            /* 复位脉宽 >10ms */
    int rst_saw_low = 0, t_low_ms = -1;
    digitalWrite(EPD_RESET_PIN, HIGH);    /* 释放复位，COG boot */
    for (int i = 0; i < 400; i++) {       /* 400ms 窗口，1ms 采样 */
        if (digitalRead(EPD_BUSY_PIN) == s_panel->busy_level) { rst_saw_low = 1; t_low_ms = i; break; }
        delay(1);
    }
    Serial.printf("[EPD-DIAG] RST pulse -> BUSY went busy-level(%d): %d (@%dms) %s\n",
                  s_panel->busy_level, rst_saw_low, t_low_ms,
                  rst_saw_low ? "-> COG ALIVE: VCI/GND/BUSY/RST all wired"
                              : "-> NO RESPONSE: check VCI 3V3 / GND / FPC / BUSY wire / RST wire");

    /* 2c. BUSY 释放跟踪（E042A13 bring-up 2026-08-22）：RST 释放后 COG
     *     自检完成应释放 BUSY（离开忙电平，极性取 desc）。持续忙 =
     *     上电异常或 BUSY 线对忙电平短路（拔屏对比可区分） */
    int t_rel_ms = -1;
    if (rst_saw_low)
        for (int i = t_low_ms; i < 600; i++) {
            delay(1);
            if (digitalRead(EPD_BUSY_PIN) != s_panel->busy_level) { t_rel_ms = i - t_low_ms; break; }
        }
    Serial.printf("[EPD-DIAG] BUSY release after RST: %s\n",
                  !rst_saw_low ? "n/a (never went busy)" :
                  t_rel_ms >= 0 ? "released (COG reset self-test done)" :
                                  "STUCK busy >600ms (COG boot stuck / BUSY shorted)");

    /* 2d. 状态读（T1.8 判族下沉：L3 不再感知控制器型号）：
     *     desc.ops.diag 由面板单元按族填写（epd_bus 标准实现
     *     bus_diag_uc / bus_diag_ssd16），IL91874 等无 FLG/版本
     *     寄存器的控制器填 NULL 跳过 */
    if (s_panel->ops.diag)
        s_panel->ops.diag();
    else
        Serial.printf("[EPD-DIAG] status read: skipped (panel has no diag op)\n");
#else
    /* 生产态（T1.8，修 E2）：bring-up 诊断序列整体不编入（体积 -4~6KB），
     * 仅保留引脚方向初始化；RST 复位与 BUSY 判活由面板 ops.init 自带
     * 序列承担（全部面板 init 均含 RST 脉冲，行为等价） */
    pinMode(EPD_BUSY_PIN, INPUT);
    pinMode(EPD_RESET_PIN, OUTPUT);
#endif

    /* 3. 硬件 SPI（EVK011 J2: SCK=pin3, SDO=pin5）。
     * GxEPD2 内部 SPI.beginTransaction 使用 GPIO matrix，任意引脚可用 */
    SPI.begin(EPD_SCK_PIN, -1, EPD_MOSI_PIN, EPD_CS_PIN);

    /* 4. 面板单元初始化（L2 ops.init：硬件复位 20ms + 初始序列，demo 时序） */
    if (s_panel->ops.init() != 0) {
        LOG_E("panel ops.init() failed");
        return -1;
    }

    /* 5. 双帧 + 画布按 desc 动态分配（Phase 2，§6.2：单帧 =
     *    panel_stride x panel_h x plane_count；BW 3.7" = 12,480B x2
     *    全 SRAM，与静态数组时代水位一致） */
    s_fb_size = (size_t)panel_stride(s_panel) * s_panel->panel_h;
    const size_t plane_bytes = s_fb_size * s_panel->plane_count;
    s_port_new  = epd_fb_alloc(plane_bytes);
    s_port_prev = epd_fb_alloc(plane_bytes);
    if (!s_port_new || !s_port_prev) {
        LOG_E("frame buffer alloc failed (%u B x2)", (unsigned)plane_bytes);
        return -1;
    }
    /* gfx 尺寸按生效旋转派生（奇数=交换，§6.3）：init 取面板默认
     * （后续 epd_set_rotation 运行期覆盖，屏幕方向设置） */
    s_rot = s_panel->gfx_rotation;
    const int gw = (s_rot & 1) ? s_panel->panel_h : s_panel->panel_w;
    const int gh = (s_rot & 1) ? s_panel->panel_w : s_panel->panel_h;
    /* 注：epd_driver.h 的 DEPG0370 镜像宏（EPD_GFX_WIDTH 系列 /
     * EPD_FB_SIZE）已删除 —— LAN 接收页同步动态化后全域零引用 */

    s_canvas = new GFXcanvas1(gw, gh);
    if (!s_canvas || !s_canvas->getBuffer()) {
        LOG_E("canvas alloc failed (%d bytes)", (gw * gh + 7) / 8);
        return -1;
    }
    s_canvas->fillScreen(CANVAS_WHITE);   /* 画布白底（与旧全刷首帧行为一致） */
    s_canvas->setTextColor(CANVAS_BLACK);
    s_canvas->setFont(s_fonts[1]);
    s_canvas->setTextWrap(false);
    /* 强调色层画布（多平面色彩面板，§9.4）：与 B/W 层同几何，bit=1=红；
     * BW 面板不分配（NULL 即单层路由，零开销零行为差异） */
    if (s_panel->plane_count > 1) {
        s_canvas_ac = new GFXcanvas1(gw, gh);
        if (!s_canvas_ac || !s_canvas_ac->getBuffer()) {
            LOG_E("accent canvas alloc failed (%d bytes)", gw * gh / 8);
            return -1;
        }
        s_canvas_ac->fillScreen(CANVAS_BLACK); /* AC 层初始无红（全 0；
                                                 * 勘误同 ac_layer_color） */
    }
    memset(s_port_prev, 0xFF, plane_bytes); /* 上一帧影子初始化为白（首次全刷前防御） */

    s_inited = true;
    LOG_I("EPD driver initialized: panel '%s' %dx%d rot=%d %s, canvas+demo-partial arch (HW SPI %d/%d)",
          s_panel->name, s_panel->panel_w, s_panel->panel_h,
          s_rot,
          s_panel->plane_count > 1 ? "dual-plane color" : "BW",
          EPD_SCK_PIN, EPD_MOSI_PIN);
    LOG_I("FB: %u B x2 (%s) + canvas %dx%d %u B x%d",
          (unsigned)plane_bytes, s_fb_in_psram ? "PSRAM" : "SRAM",
          gw, gh, (unsigned)((gw * gh + 7) / 8), s_canvas_ac ? 2 : 1);
    /* dpi 诊断行（2026-09-03）：物理字高 mm = px÷dpi×25.4，新屏上机
     * 对照现役基线（3.7" 130PPI：16px≈3.1mm / 20px≈3.9mm / 24px≈4.7mm）
     * 判「档内但视觉不符」的同档异 PPI 风险。2026-09-08 起同步注入
     * layout_profile PPI 自动层（主内容/释义字号按物理字高选级，
     * set_dpi 在 init 内早于 UI 首调 get，时序保证） */
    layout_profile_set_dpi(s_panel->dpi);
    LOG_I("Panel dpi=%u (16px=%.1fmm 20px=%.1fmm 24px=%.1fmm 32px=%.1fmm)",
          s_panel->dpi,
          16.0f * 25.4f / s_panel->dpi, 20.0f * 25.4f / s_panel->dpi,
          24.0f * 25.4f / s_panel->dpi, 32.0f * 25.4f / s_panel->dpi);
#if defined(INKWORD_BOARD_V14)
    LOG_I("Booster: v1.4 on-board self-managed boost (decoupled from COG GDR), no MCU PWM");
#else
    LOG_I("Booster: EVK011 discrete boost driven by panel COG (GDR on FPC pin2), no MCU PWM");
#endif
    return 0;
}

void epd_power_on(void)
{
    /* 电源由屏幕 COG 在 0x04 命令后自主升压，此处仅保证 SPI/引脚就绪 */
    if (!s_inited) return;
    LOG_D("EPD power ON requested (panel COG self-managed)");
}

void epd_power_off(void)
{
    if (!s_inited) return;
    s_panel->ops.power_off(); /* 0x02 关高压 rails */
    LOG_D("EPD power OFF (0x02 sent)");
}

void epd_clear_screen(void)
{
    if (!s_inited) return;
    /* BW 面板：黑白交替一轮再回白——仅白帧全刷对长时间驻留的深色像素
     * 翻转不彻底（真机验证：旧布局时钟数小时局刷后，开机白屏全刷仍
     * 留残影），先全黑全刷把陈年黑迹充分翻转再回白；调用点均为低频
     * 路径（开机白屏 / 长按清残影 / 局刷阈值），多一次全刷可接受。
     * 三色面板：无局刷即无残影累积（§13.2），且全刷 16s，黑白交替
     * 双刷成本不可接受 → 单次白清 */
    if (s_panel->color_mode == EPD_COLOR_BW) {
        epd_gfx_fill_screen(EPD_GFX_BLACK);   /* 经 API 路由：同步清 AC 层 */
        epd_gfx_flush();
    }
    epd_gfx_fill_screen(EPD_GFX_WHITE);
    epd_gfx_flush();
    LOG_D("EPD deep clear done");
}

void epd_full_refresh(const uint8_t *data)
{
    if (!s_inited) {
        LOG_E("EPD not initialized");
        return;
    }

    /* data：竖屏整帧（面板物理 240x416，行宽 30，bit=1 白/0=黑 COG 原生语义），
     * 直通 epd2 层不做旋转；NULL 时清白。
     * 注意：UI 主路径请用 epd_gfx_*（横屏 GFX 坐标）；
     * 本路径不更新 s_port_prev（LAN 直传后调用方须强制下一次全刷，
     * main.cpp ui_force_full_refresh_next() 已保证）。
     * T0.1 加锁：与按键任务局刷序列互斥（互斥锁非递归，勿在持锁路径内
     * 再调本函数） */
    if (!epd_lock()) {
        LOG_E("epd lock timeout, full_refresh dropped");
        return;
    }
    s_panel->ops.write_full(data);
    epd_unlock();

    LOG_D("EPD full refresh done");
}

/* epd_partial_refresh 已删（2026-08-20）：窗口直通路径（drawImagePart）
 * 属已禁用的 partial window 方案，且不维护 s_port_prev，误用会破坏
 * 无窗口双 RAM 差分的前帧一致性；UI 局刷统一走 epd_gfx_flush_window* */

void epd_deep_sleep(void)
{
    if (!s_inited) return;

    /* T0.1 加锁：确保无并发刷新进行中再下电（锁被 LAN 全刷持有时，
     * 在此等其完成——入睡宁晚勿错） */
    if (!epd_lock()) {
        LOG_W("epd lock timeout before deep sleep, proceed anyway");
    }
    s_panel->ops.deep_sleep(); /* 0x02 下电 + 0x07/0xA5 深睡，可被硬件复位唤醒 */
    epd_unlock();

    /* 注意：保持 s_inited=true —— hibernate 后置 _init_display_done=false，
     * 下一次局刷路径的 hwReset()/写数据前会自动复位并重新初始化 */
    LOG_I("EPD entered deep sleep (wake by reset)");
}

uint16_t epd_get_manufacturer(char *manufacturer, size_t len)
{
    /* 面板化（Phase 6）：desc.name 下划线前段即厂商段（如 "depg0370_"
     * → "depg0370"），返回 controller 枚举值作 ID；未初始化空串 + 0 */
    if (!s_panel) {
        if (manufacturer && len > 0) manufacturer[0] = '\0';
        return 0;
    }
    if (manufacturer && len > 0) {
        snprintf(manufacturer, len, "%s", s_panel->name);
        char *us = strchr(manufacturer, '_');
        if (us) *us = '\0';
    }
    return (uint16_t)s_panel->controller;
}

int epd_set_rotation(uint8_t rot)
{
    if (!s_inited) {
        LOG_E("set_rotation before driver init");
        return -1;
    }
    if (rot > 3) {
        LOG_W("set_rotation: invalid rot %u (expect 0..3)", rot);
        return -1;
    }
    if (rot == s_rot) return 0;   /* 幂等：含「意图映射回面板默认」路径 */

    const int gw = (rot & 1) ? s_panel->panel_h : s_panel->panel_w;
    const int gh = (rot & 1) ? s_panel->panel_w : s_panel->panel_h;

    /* T0.1 加锁：画布重建全程持锁（delete 后 new 前的窗口期内，并发
     * 刷新路径的 canvas_to_panel 会读到悬垂指针） */
    if (!epd_lock()) {
        LOG_E("epd lock timeout, rotation change dropped");
        return -1;
    }

    /* 先建后换（失败路径原画布完好，渲染无损）：初始化与
     * epd_driver_init 同参——白底/黑字/14pt/不折行，AC 层全 0 无红 */
    GFXcanvas1 *cv = new GFXcanvas1(gw, gh);
    if (!cv || !cv->getBuffer()) {
        delete cv;
        epd_unlock();
        LOG_E("rotation canvas alloc failed (%dx%d)", gw, gh);
        return -1;
    }
    GFXcanvas1 *cv_ac = NULL;
    if (s_panel->plane_count > 1) {
        cv_ac = new GFXcanvas1(gw, gh);
        if (!cv_ac || !cv_ac->getBuffer()) {
            delete cv;
            delete cv_ac;   /* delete NULL 安全 */
            epd_unlock();
            LOG_E("rotation accent canvas alloc failed (%dx%d)", gw, gh);
            return -1;
        }
    }
    delete s_canvas;
    delete s_canvas_ac;       /* BW 面板常态 NULL，delete NULL 安全 */
    s_canvas    = cv;
    s_canvas_ac = cv_ac;
    s_canvas->fillScreen(CANVAS_WHITE);
    s_canvas->setTextColor(CANVAS_BLACK);
    s_canvas->setFont(s_fonts[1]);
    s_canvas->setTextWrap(false);
    if (s_canvas_ac) s_canvas_ac->fillScreen(CANVAS_BLACK);
    s_rot = rot;
    epd_unlock();

    /* s_port_prev 面板物理帧快照保持有效（旋转不改屏幕物理内容）；
     * 新画布白底与屏幕旧内容不一致属正常初态，调用方首次全刷覆盖 */
    LOG_I("rotation -> %d, canvas rebuilt %dx%d", rot, gw, gh);
    return 0;
}

uint8_t epd_get_rotation(void)
{
    return s_inited ? s_rot : 0;
}

uint8_t epd_panel_default_rotation(void)
{
    return s_panel ? s_panel->gfx_rotation : 0;
}

} /* extern "C" */

/* ============================================================
 * C-callable GFX 包装（画布直通，y 基线语义与 FreeSans 字体一致）
 * ============================================================ */

extern "C" {

int epd_gfx_width(void)  { return s_canvas ? s_canvas->width() : 0; }
int epd_gfx_height(void) { return s_canvas ? s_canvas->height() : 0; }

void epd_gfx_fill_screen(uint16_t color)
{
    if (!s_canvas) return;
    s_canvas->fillScreen(bw_layer_color(color));
    if (s_canvas_ac) s_canvas_ac->fillScreen(ac_layer_color(color));
}

void epd_gfx_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_canvas) return;
    s_canvas->fillRect(x, y, w, h, bw_layer_color(color));
    if (s_canvas_ac) s_canvas_ac->fillRect(x, y, w, h, ac_layer_color(color));
}

void epd_gfx_draw_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_canvas) return;
    s_canvas->drawRect(x, y, w, h, bw_layer_color(color));
    if (s_canvas_ac) s_canvas_ac->drawRect(x, y, w, h, ac_layer_color(color));
}

void epd_gfx_draw_hline(int x, int y, int w, uint16_t color)
{
    if (!s_canvas) return;
    s_canvas->drawFastHLine(x, y, w, bw_layer_color(color));
    if (s_canvas_ac) s_canvas_ac->drawFastHLine(x, y, w, ac_layer_color(color));
}

void epd_gfx_draw_vline(int x, int y, int h, uint16_t color)
{
    if (!s_canvas) return;
    s_canvas->drawFastVLine(x, y, h, bw_layer_color(color));
    if (s_canvas_ac) s_canvas_ac->drawFastVLine(x, y, h, ac_layer_color(color));
}

void epd_gfx_draw_text(int x, int y, const char *text, uint16_t color, int font_size)
{
    if (!text || !s_canvas) return;

    /* FreeSans 无 CJK 字形，超出 Latin-1 的字符将无法渲染 */
    for (const char *p = text; *p; p++) {
        if ((uint8_t)*p > 0x7F) {
            LOG_W("draw_text: non-ASCII text (CJK needs U8g2 font): '%s'", text);
            break;
        }
    }

    s_canvas->setFont(font_for_size(font_size));
    s_canvas->setTextColor(bw_layer_color(color));
    s_canvas->setCursor(x, y); /* FreeSans: y 为基线 */
    s_canvas->print(text);
    if (s_canvas_ac) {
        s_canvas_ac->setFont(font_for_size(font_size));
        s_canvas_ac->setTextColor(ac_layer_color(color));
        s_canvas_ac->setCursor(x, y);
        s_canvas_ac->print(text);
    }
}

void epd_gfx_text_bounds(const char *text, int font_size, int *out_w, int *out_h)
{
    if (!out_w || !out_h) return;
    if (!text || !s_canvas) {
        *out_w = 0;
        *out_h = 0;
        return;
    }
    int16_t x1, y1;
    uint16_t w, h;
    s_canvas->setFont(font_for_size(font_size));
    s_canvas->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    *out_w = w;
    *out_h = h;
}

void epd_gfx_draw_bitmap(int x, int y, int w, int h, const uint8_t *bits, uint16_t color)
{
    if (!bits || !s_canvas) return;
    /* bits：行主序 MSB-first（每行 ceil(w/8) 字节），bit=1 画 color，0 透明 */
    s_canvas->drawBitmap(x, y, bits, w, h, bw_layer_color(color));
    if (s_canvas_ac) s_canvas_ac->drawBitmap(x, y, bits, w, h, ac_layer_color(color));
}

void epd_gfx_read_window(int x, int y, int w, int h, uint8_t *out)
{
    if (!out || !s_canvas) return;
    /* 与 draw_bitmap 输入格式互补的窗口提取：行主序 MSB-first，每行
     * ceil(w/8) 字节，bit=1 = 画布置位 = 黑。逐行逐像素从画布 stride
     * ((gw+7)/8 字节/行) 装配，窗口超界部分置 0（白）。
     * 注：仅读 B/W 层（差分影子统计语义，§9.4）——ACCENT 层像素
     * 不会被计数，上层差分分流对纯黑白场景（待机引文）语义完整 */
    const int gw = s_canvas->width(), gh = s_canvas->height();
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > gw) w = gw - x;
    if (y + h > gh) h = gh - y;
    if (w <= 0 || h <= 0) return;

    const uint8_t *src = s_canvas->getBuffer();
    const int stride = (gw + 7) / 8;
    const int wbytes = (w + 7) / 8;
    memset(out, 0, (size_t)wbytes * h);
    for (int cy = 0; cy < h; cy++) {
        const uint8_t *row = src + (uint32_t)(y + cy) * stride;
        uint8_t *dst = out + (uint32_t)cy * wbytes;
        for (int cx = 0; cx < w; cx++) {
            if (row[(x + cx) >> 3] & (0x80 >> ((x + cx) & 7)))
                dst[cx >> 3] |= (uint8_t)(0x80 >> (cx & 7));
        }
    }
}

/* 全刷主体（T0.1 提取：调用方已持锁；供 epd_gfx_flush 与
 * flush_window 的 partial_enabled=false 降级分支复用，避免
 * 非递归互斥锁嵌套加锁死锁） */
static void gfx_flush_locked(void)
{
    canvas_to_panel(s_port_new);
    /* 真全刷（demo 忠实序列，实现在 panels/panel_depg0370_uc8253.cpp：
     * 硬复位→full 初始化→无窗口整屏写 0x13→0x04/0x12/0x02） */
    s_panel->ops.full_refresh(s_port_new);
    memcpy(s_port_prev, s_port_new, s_fb_size * s_panel->plane_count); /* 屏幕内容 == 新帧 */
}

void epd_gfx_flush(void)
{
    if (!s_inited) return;
    if (!epd_lock()) {
        LOG_E("epd lock timeout, flush dropped");
        return;
    }
    gfx_flush_locked();
    epd_unlock();
}

void epd_gfx_flush_window_passes(int x, int y, int w, int h, int passes)
{
    if (!s_inited) return;

    uint16_t px, py, pw, ph;
    gfx_rect_to_panel(x, y, w, h, &px, &py, &pw, &ph);
    if (pw == 0 || ph == 0) return; /* 窗口参数仅做区域合法性检查 */

    /* T0.1：局刷序列与 LAN 直刷/旋转重建互斥（含窗口转置读画布段） */
    if (!epd_lock()) {
        LOG_E("epd lock timeout, window flush dropped");
        return;
    }

    /* 色彩面板无快速局刷（§13.2：full==partial 且无差分波形）：整帧
     * 走全刷——画布为累积缓冲，整帧推送与窗口语义一致，调用方零改动。
     * 全刷 16s 的刷新计费约束由上层 UX 降级承担（待机自动轮换停用）。
     * 调 gfx_flush_locked（已持锁，勿再调 epd_gfx_flush） */
    if (!s_panel->partial_enabled) {
        LOG_I("no fast partial on '%s': window refresh -> full (~%ums)",
              s_panel->name, (unsigned)s_panel->full_ms);
        gfx_flush_locked();
        epd_unlock();
        return;
    }

    canvas_to_panel(s_port_new);

    /* Plan B：无窗口整屏双 RAM 差分局刷（序列实现与完整实测记录见
     * panels/panel_depg0370_uc8253.cpp panel_partial 注释） */
    s_panel->ops.partial(s_port_prev, s_port_new,
                         (uint8_t)(passes < 1 ? 1 : passes));

    memcpy(s_port_prev, s_port_new, s_fb_size * s_panel->plane_count); /* 屏幕内容 == 新帧 */
    epd_unlock();
}

bool epd_gfx_partial_supported(void)
{
    /* desc.partial_enabled 透传：上层 UX 降级判据（§13.2，如待机页
     * 三色屏自动轮换停用）；未初始化时保守 false */
    return s_panel ? s_panel->partial_enabled : false;
}

const epd_panel_desc_t *epd_panel_desc(void)
{
    /* 当前 desc 只读透传：上层读刷新策略字段（保养阈值/passes 等）
     * 统一走 desc，消除硬编码与面板配置脱钩（wft0290 调优实锄：
     * flush_window 硬编码 2 + scheduler 硬编码 8 双坑） */
    return s_panel;
}

size_t epd_fb_size(void)
{
    /* 单平面帧字节（LAN 协议帧大小；行宽字节向上取整，OPM021EB
     * 122px → 4,000B）；未初始化返回 0 */
    return s_panel ? (size_t)panel_stride(s_panel) * s_panel->panel_h : 0;
}

size_t epd_fb_total(void)
{
    /* 全平面整帧字节（外部直刷缓冲容量，多平面色彩面板含红平面） */
    return s_panel ? (size_t)panel_stride(s_panel) * s_panel->panel_h
                     * s_panel->plane_count : 0;
}

int epd_panel_width(void)
{
    return s_panel ? s_panel->panel_w : 0;
}

uint32_t epd_panel_accent_rgb(void)
{
    return s_panel ? s_panel->accent_rgb : 0;
}

int epd_panel_height(void)
{
    return s_panel ? s_panel->panel_h : 0;
}

void epd_gfx_flush_window(int x, int y, int w, int h)
{
    /* 默认遍数取面板 desc.passes（此前硬编码 2 使字段形同虚设）：
     * DEPG0370=2 双刷保净先例；wft0290=1 对照实验定位「翻页回退」
     * （单相 REG LUT 第二遍同向过驱动疑似伪影源，详见该面板 desc）。
     * 显式 _passes 版本（菜单/WiFi/待机页）不受影响 */
    epd_gfx_flush_window_passes(x, y, w, h,
                                s_panel && s_panel->passes > 0
                                    ? s_panel->passes : 1);
}

} /* extern "C" for GFX wrappers */
