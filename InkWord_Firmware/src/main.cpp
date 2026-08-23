/**
 * @file main.cpp
 * @brief 应用主入口 (Task F-20)
 *
 * 启动流程：日志 -> NVS -> 存储(SD) -> 屏幕 -> 音频 -> 按键 ->
 *          WiFi -> 词库 -> 进入上次模式 -> 主循环（事件驱动）。
 *
 * 五向导航按键映射（2026-08 取代 6 独立按键；SET/RST 侧键同月接入）：
 *   上 短按=上一条（释义多页时先翻上一释义页）/ 长按=清残影全刷；
 *   下 短按=下一条（释义多页时先翻下一释义页）/ 长按=切换学习模式；
 *   中 短按=发音 / 长按=进入功能菜单（快捷菜单 menu_ui：收藏/模式/
 *        配网/门户/LAN/设备信息；Wi-Fi 配网降为菜单项，2026-08-23）；
 *   左 短按=自评「忘记」Q1（SM-2 质量分 1：连错+1，>0 入错词本）/ 长按=进入 AP 直连/配网门户
 *        （手机连 InkWord-Setup 热点直传，绕开路由器隔离；任意键退出）；
 *   右 短按=自评「简单」Q5（SM-2 质量分 5：连错清零，错词本中移出）/ 长按=进入 LAN 接收页（同网浏览器直传，任意键退出）；
 *   SET 短按=遮蔽/揭晓释义（闪卡自测；待机页=轮换下一条引文）/ 长按=收藏/取消当前词（左栏 * 标记；收藏视图内=移出序列）；
 *   RST 短按=回到当前模式第一条 / 长按=临时视图进出（错词本或收藏浏览，
 *        按当前所在视图退出，否则进错词本）。
 *
 * 阅读模式（P3，长按下循环切换进入）：上/下=翻页，左/右=字号缩放
 * （16/20/24px 三级循环，按当前页首字符就近保持阅读位置），RST=回
 * 第一页；中/SET 短按与词相关长按（收藏）不适用；进度自动保存
 * （NVS rd_*）。
 *
 * 无词库待机页（词库为空时默认显示，见 standby_page.c）：
 *   时钟/日历/天气整页；待机态长按语义与学习页一致（功能菜单/清残影/
 *   门户/LAN），短按中=立即拉取天气，其余短按忽略。
 *
 * 深睡与定时唤醒（P5，power_manager.c）：无操作 10 分钟入睡（引文轮换/
 * 后台心跳随交互模式冻结，墨水屏驻留末帧零功耗）；中键唤醒恢复交互
 * （自治钟 RTC 慢钟差分恢复 + 联网 HTTP Date 校准兜底）；RTC TIMER
 * 每 2h 静默心跳会话（Wi-Fi 快连 → 校时 → 上报/心跳/OTA → 回睡，
 * 全程不碰屏）。唤醒即重启，setup 最早期按唤醒原因分流。
 */
#include <Arduino.h>

#include "debug_log.h"
#include "gpio_config.h"
#include "epd_driver.h"
#include "audio_player.h"
#include "button_handler.h"
#include "haptic.h"
#include "storage_manager.h"
#include "refresh_scheduler.h"
#include "word_parser.h"
#include "cjk_text.h"     /* 词卡释义/tag 中文点阵混排（P3 字库资产） */
#include "layout_profile.h" /* 布局档位：SMALL 单列 / MID 双栏分档（§8.1） */
#include "srs_engine.h"
#include "learning_state.h"
#include "study_mode_machine.h"
#include "wifi_manager.h"
#include "wifi_config_ui.h"
#include "menu_ui.h"      /* 快捷菜单（功能菜单，长按中进入） */
#include "sync_client.h"
#include "ota_manager.h"
#include "lan_display_server.h"
#include "standby_page.h"
#include "reader_engine.h"   /* 阅读模式（P3）：书分页/字号/进度 */
#include "ble_provision.h"
#include "power_manager.h"   /* P5 深睡/唤醒分流与入睡检查 */

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"    /* 词池/阅读器书缓冲：PSRAM 分配 */
#include "esp_mac.h"          /* esp_read_mac：首次注册的设备身份 */
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "MAIN";
#define FW_VERSION  "1.0.0"
/* 固件版本 getter（快捷菜单设备信息页跨模块取用；FW_VERSION 为文件内宏） */
extern "C" const char *fw_version(void) { return FW_VERSION; }
/* 词库容量（PRD §7.2 容量红线 2026-08-20 解除）：词池迁 PSRAM 后
 * 上限 4000 词（词库扩展四字段后 sizeof(WordEntry)≈1096B，
 * 4000 词 ≈ 4.2MB；与阅读器单书上限 4MB 并发最坏 ≈ 8.2MB——仅
 * “满词库+4MB 大书”同时存在时才触顶，实际书多在 1-2MB 且词池
 * 分配失败时逐半降级兼容）。实际分配不足时 setup 内逐级降级，
 * 见 s_word_pool 分配处 */
#define MAX_WORDS   4000

/* 后端 API Base URL（P2 上报闭环）：部署时 -D INKWORD_API_BASE=... 覆盖，
 * 或经 NVS "inkword"/"api_url" 覆盖（配网 UI 扩展后可写）；
 * http: 前缀自动走明文 TCP（本地开发后端，见 sync_client fill_cfg） */
#ifndef INKWORD_API_BASE
#define INKWORD_API_BASE "https://api.einkword.com"
#endif

/* 演示词库开关：inkword-s3-demo 环境置 1；无 SD 词库时加载内嵌 5 词，
 * 用于学习页按键（翻词/SET 遮蔽/RST 回首）的整机验证；
 * 正式构建保持 0，无词库仍走待机页（用户定稿行为） */
#ifndef INKWORD_DEMO_WORDS
#define INKWORD_DEMO_WORDS 0
#endif

/* BLE 配网服务默认禁用：Arduino 预编译库未编入 Wi-Fi/BLE coexistence
 * （CONFIG_ESP32_WIFI_SW_COEXIST_ENABLE 编译期固定关闭），Wi-Fi controller
 * 活动时 esp_bt_controller_enable 经 coex_enable() abort（真机崩溃循环）。
 * 迁移到可开 coex 的构建（IDF 框架/自编译 libs）后置 1 启用。 */
#ifndef INKWORD_BLE_PROVISION
#define INKWORD_BLE_PROVISION 0
#endif

/* 词池：PSRAM 堆分配（原 DRAM 静态数组仅容 64 词；与 reader_engine
 * 书缓冲同策略 MALLOC_CAP_SPIRAM，setup 内 storage_init 后分配） */
static WordEntry *s_word_pool = NULL;
static int        s_word_cap = 0;   /* 实际分配容量（降级后 < MAX_WORDS） */

/* ============================================================
 * 单词卡片 UI 渲染 + 局部刷新策略 (Task F-16)
 *
 * 布局按屏分档（PANEL_COMPAT_DESIGN §8.1；FreeSans 基线 y / 点阵顶左 y）：
 *   MID 双栏（视觉基线 416x240，rotation=1；4.2" 400x300 同族）：
 *     y[0,32)    状态栏：模式名（左）/ 序号（右）/ 分隔线
 *     左栏 x[16,248)  单词(24pt 超宽自动降级) + 音标(9pt) + 底部标签
 *                    （tag 含中文时走 16px 点阵，见 cjk_text）
 *     竖分隔线 x=248；右栏 x[264,400) 释义(16px 点阵混排自动断行，
 *                    行数按屏高派生，超出一屏分页上下键词内翻页；
 *                    中文释义可渲染——真实词库释义为中文，FreeSans 仅 ASCII)
 *   SMALL 单列（短边 <200px，2.7" 264x176 首例，2026-08-23 接入）：
 *     头部单词(全宽自适应)+音标固定 → 释义+词根全宽分页正文流 →
 *     底部标签行（双栏右栏仅 75px ≈ 4 字/行不可用，故降单列）
 *
 * 刷新策略（2026-08-20 无窗口方案定稿，见 README「局部刷新方案」）：
 *   - 首帧 / 模式切换 / 保养：整屏重绘 + 全刷（epd_gfx_flush）
 *   - 同模式翻词/翻页：重绘内容区 + 局刷（epd_gfx_flush_window，
 *     无窗口整屏双 RAM 差分：整屏写 0x10 旧帧 + 0x13 新帧，COG
 *     全屏差分只翻转变化像素，无闪烁；状态栏不重绘自动跳过）
 *   - 残影管理：局刷计数达阈值（学习/阅读页 8 次）时升级为整屏重绘
 *     + 真全刷低频保养（局刷自身无残影，全刷仅防累积）
 * ============================================================ */

/* Phase 4 去硬编码：位置类宏由 epd_gfx_width()/height() 运行期派生
 * （416x240 下与旧字面精确相等，视觉零变化）；尺寸/行距类保留语义
 * 常量（与字体档联动，Phase 5/档位 profile 参数化）。完整四档布局
 * 参数表（layout_profile）见 PANEL_COMPAT_DESIGN §8.1，SMALL/LARGE
 * 档实际接入时再建。 */
/* LAYOUT_TINY（2026-08-23 新增档：2.13" 122x250 / 2.9" 128x296 电子
 * 标签屏竖持）：超紧凑头部——状态栏 24、边距 8、头部行距收紧，
 * 正文 level 0 同 SMALL；竖屏高向充裕（250/296px 存 7/9 行） */
#define UI_TINY         (layout_profile_get()->kind == LAYOUT_TINY)
#define UI_STATUS_H     (UI_TINY ? 24 : 32)  /* 状态栏高度（内容区顶 y；无窗口差分下不再要求 8 对齐） */
#define UI_MARGIN_X     (UI_TINY ? 8 : 16)   /* 左右留白 */
#define UI_STATUS_BASE  (UI_STATUS_H - 10)             /* 状态栏文字基线（22/14） */
/* ---- 学习页单列版式（全档位统一，2026-08-23 重设计）----
 * 上下结构：头部单词（全宽大字自适应）+ 音标 + 收藏星标；正文流 =
 * 释义+词根全宽分页；底部标签行全宽。双栏版式退役（真机反馈 136px
 * 右栏 7 字/行阅读体验差，416x240 全宽 21 字/行提升 3 倍）；
 * 字号档位派生：TINY/SMALL 16px / MID+ 20px */
#define UI_MEAN_LEVEL   (layout_profile_get()->kind <= LAYOUT_SMALL \
                         ? 0 : 1)  /* 正文字号级：TINY/SMALL 16px / 其余 20px */
#define UI_WORD_BASE    (UI_STATUS_H + (UI_TINY ? 24 \
                         : (UI_MEAN_LEVEL ? 36 : 32)))  /* 单词基线（68/64/48） */
#define UI_PHON_TOP     (UI_WORD_BASE + (UI_TINY ? 6 : 9))     /* 音标行 16px 点阵顶（77/73/54） */
#define UI_BODY_TOP     (UI_PHON_TOP + 16 + (UI_TINY ? 4 : 11)) /* 正文流首行顶（104/100/74） */
#define UI_BODY_LH      (UI_MEAN_LEVEL ? 24 : 20)  /* 正文行距：字级 +4（reader 惯例） */
/* 行数按屏高派生：底部预留 30 = 标签行 + 余量（末行文字底与标签顶
 * 错开，MID 末行底 196 < 标签顶 206）；416x240=4 行、400x300=6、
 * 264x176=2、122x250 竖屏=7、128x296 竖屏=9（TINY 预留收至 26） */
#define UI_BODY_RESERVE (UI_TINY ? 26 : 30)
#define UI_BODY_LINES   ((epd_gfx_height() - UI_BODY_RESERVE - UI_BODY_TOP) / UI_BODY_LH)
#define UI_BODY_MAX_W   (epd_gfx_width() - 2 * UI_MARGIN_X)   /* 全宽正文（392/232/106/112） */
#define UI_FOOT_BASE    (epd_gfx_height() - 16)        /* 底部标签基线：底边距 16（224） */
#define UI_FOOT_TOP     (UI_FOOT_BASE - 18)             /* 中文 tag 16px 点阵顶：基线上 16+2（206） */

/* 音标行 16px 点阵整行渲染（2026-08-23 IPA 修复，08-24 记号补全）：
 * 原方案 FreeSans（仅 0x20-0x7E）逐字跳过非 ASCII——真机 'ˈizi'
 * 重音符丢失，曾以 phonetic_ascii 转 ASCII 近似（ə→e 发音错位）；
 * 现字库收录 IPA 21 字符（STHeitiSC-Medium 点阵，gen_cjk_font.swift
 * 分派渲染）+ 诗词词条作者名/中点（default_words.json phonetic 列
 * 全量收集），词池原始 IPA 直渲。08-24 补全：① 词典惯例斜杠包裹
 * ——词库 phonetic 为裸 IPA（无 / /），显示层条件补齐（自带 / 或 [ ]
 * 的云端/SD 词库不双包）；② ˈ ˌ ː · 四记号生成器合成位图（字体
 * 渲染 16px 级 1px 细笔低于阈值被丢弃，重音符曾显为空格）；
 * 未收录字符画 cell 空心框兜底 */
static study_mode_t s_last_mode = MODE_COUNT; /* 无效值：首帧强制全刷 */

/* 释义分页游标（2026-08-23）：与词绑定——换词/换模式（含错词本进出、
 * RST 回首、自评移词）给 ui_render_word 检测到词变化即归零，同词
 * SET 翻义保持页位；页数由排版几何实时派生（见 ui_mean_total_pages） */
static int s_mean_page = 0;      /* 当前释义页（0 基） */
static int s_mean_word = -1;     /* 页游标绑定的词库索引（错词本=映射后） */

/* 字号自适应：从 start_size 逐级降到能放进 max_w 的字号 */
static int ui_fit_font(const char *text, int start_size, int max_w)
{
    int tw, th;
    for (int fs = start_size; fs >= 1; fs--) {
        epd_gfx_text_bounds(text, fs, &tw, &th);
        if (tw <= max_w) return fs;
    }
    return 1;
}

/* ============================================================
 * 释义分页基建（2026-08-23）：排版几何按屏幕尺寸/档位运行期派生，
 * 释义超一屏分页，上下键词内翻页（边界处交状态机翻词）。
 * 量测（cjk_text_wrap_lines）与绘制（cjk_text_draw_wrap_page）共用
 * 同一断行核心，页数与渲染行严格一致（reader_engine 建页同策略）。
 * ============================================================ */
extern "C" void ui_render_word(study_mode_t mode, int index);  /* 下方定义 */
extern "C" void ui_render_current(void);  /* 下方定义（menu_ui 恢复退出用） */

/* 释义正文流（全档位单列）：释义 + 全角空格(U+3000) + 词根（单列无
 * 独立词根槽，词根随释义滚动分页；空释义时前导空白被断行核心
 * 行首吞掉） */
static const char *ui_body_stream(const WordEntry *w)
{
    if (!w->root[0])
        return w->meaning;
    static char stream[WORD_MEANING_MAX + WORD_ROOT_MAX + 8];
    snprintf(stream, sizeof(stream), "%s\xE3\x80\x80%s",
             w->meaning, w->root);
    return stream;
}

/* 页数 = 总行数向上取整 / 每页行数（空文 1 页兜底） */
static int ui_page_count(const char *s, int max_w, int lines)
{
    int need = cjk_text_wrap_lines(max_w, UI_MEAN_LEVEL, s);
    int pages = (need + lines - 1) / lines;
    return pages < 1 ? 1 : pages;
}

/* 释义区几何（单列统一，档位字号派生）：全宽正文流；strip>0 表示
 * 多页时页码指示与正文首行同行，正文右侧须预留指示条并按缩窄宽度
 * 重建页数（量测与绘制同宽，断行一致）；ind_* = 指示器右缘 x / 基线 y */
static void ui_mean_geom(int *x, int *top, int *max_w, int *lh, int *lines,
                         int *ind_rx, int *ind_by, int *strip)
{
    int n = UI_BODY_LINES;
    if (n < 1) n = 1;
    *x = UI_MARGIN_X; *top = UI_BODY_TOP;
    *max_w = UI_BODY_MAX_W; *lh = UI_BODY_LH;
    *lines = n;
    *ind_rx = epd_gfx_width() - UI_MARGIN_X;
    *ind_by = UI_BODY_TOP + (UI_MEAN_LEVEL ? 20 : 14); /* 正文首行基线（右缘同行） */
    /* 多页页码指示条预留：TINY 加宽（106px 正文下每行仅 3~4 字，
     * 页数易破十→“10/11” 5 字符 size1 ≈40px>30 会压首行末字） */
    *strip = UI_TINY ? 44 : 30;
}

/* 当前布局下释义流总页数（含 SMALL 指示条缩窄重建，与绘制同口径） */
static int ui_mean_total_pages(const WordEntry *w)
{
    int x, top, max_w, lh, lines, ind_rx, ind_by, strip;
    ui_mean_geom(&x, &top, &max_w, &lh, &lines, &ind_rx, &ind_by, &strip);
    const char *s = ui_body_stream(w);
    int pages = ui_page_count(s, max_w, lines);
    if (pages > 1 && strip > 0)
        pages = ui_page_count(s, max_w - strip, lines);
    return pages;
}

/* 释义分页渲染：页数派生 + 游标钳位 + 当前页绘制 + 多页页码指示 */
static void ui_draw_mean_paged(const WordEntry *w)
{
    int x, top, max_w, lh, lines, ind_rx, ind_by, strip;
    ui_mean_geom(&x, &top, &max_w, &lh, &lines, &ind_rx, &ind_by, &strip);
    const char *s = ui_body_stream(w);

    int pages = ui_page_count(s, max_w, lines);
    if (pages > 1 && strip > 0) {        /* 正文缩窄重建页数（同宽一致） */
        max_w -= strip;
        pages = ui_page_count(s, max_w, lines);
    }
    if (s_mean_page >= pages) s_mean_page = pages - 1;
    if (s_mean_page < 0) s_mean_page = 0;

    cjk_text_draw_wrap_page(x, top, max_w, UI_MEAN_LEVEL, lh, lines,
                            s_mean_page, s, EPD_GFX_BLACK);

    if (pages > 1) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%d/%d", s_mean_page + 1, pages);
        int tw, th;
        epd_gfx_text_bounds(buf, 1, &tw, &th);
        epd_gfx_draw_text(ind_rx - tw, ind_by, buf, EPD_GFX_BLACK, 1);
    }
}

/* 上下键词内翻页：当前词释义多页且未越界时翻释义页（true=已消费）；
 * 单页/遮蔽态/越界（首尾页）返回 false 交状态机翻词——与阅读模式
 * 「上下=翻页」游标语义同族，翻词后页游标自动归零 */
static bool ui_mean_page_step(int dir)
{
    if (study_mode_current() == MODE_READER) return false;
    if (!study_mode_is_revealed()) return false;   /* 遮蔽自测态无页可翻 */

    const WordEntry *w = word_parser_get(study_mode_current_word_index());
    if (!w) return false;

    int np = s_mean_page + dir;
    if (np < 0 || np >= ui_mean_total_pages(w)) return false;
    s_mean_page = np;
    ui_render_word(study_mode_current(), study_mode_current_word_index());
    return true;
}

/* 绘制状态栏：模式名（左）+ 序号（右，错词本=序号/错词数）+ 分隔线 */
static void ui_draw_status(study_mode_t mode)
{
    epd_gfx_fill_rect(0, 0, epd_gfx_width(), UI_STATUS_H, EPD_GFX_WHITE);

    /* 模式名：中文（收藏视图）走 16px 点阵（顶左坐标垂直居中），
     * ASCII 模式名保持 FreeSans 基线路径视觉不变 */
    const char *name = study_mode_name(mode);
    if (cjk_text_has_wide(name))
        cjk_text_draw(UI_MARGIN_X, (UI_STATUS_H - 16) / 2,
                      0, name, EPD_GFX_BLACK);
    else
        epd_gfx_draw_text(UI_MARGIN_X, UI_STATUS_BASE,
                          name, EPD_GFX_BLACK, 1);

    char buf[24];
    int total = study_mode_seq_total();
    int tw, th;
    snprintf(buf, sizeof(buf), "%d/%d",
             total ? study_mode_seq_pos() + 1 : 0, total);
    epd_gfx_text_bounds(buf, 1, &tw, &th);
    epd_gfx_draw_text(epd_gfx_width() - UI_MARGIN_X - tw, UI_STATUS_BASE,
                      buf, EPD_GFX_BLACK, 1);

    epd_gfx_draw_hline(UI_MARGIN_X, UI_STATUS_H,
                       epd_gfx_width() - 2 * UI_MARGIN_X, EPD_GFX_BLACK);
}

/* 底部标签行：tag·grade·source 非空项以间隔号拼接（间隔号在
 * 字库全角标点集内）；含中文走 16px 点阵单行截断，纯 ASCII
 * 且无扩展字段时保持 FreeSans 9pt（max_w：双栏=左栏宽，单列=全宽） */
static void ui_draw_foot(const WordEntry *w, int max_w)
{
    char foot[160];
    int  fl = 0;
    const char *parts[3] = { w->tag, w->grade, w->source };
    for (int i = 0; i < 3 && fl < (int)sizeof(foot) - 2; i++) {
        if (!parts[i][0]) continue;
        if (fl > 0) { foot[fl++] = '\xC2'; foot[fl++] = '\xB7'; } /* U+00B7 间隔号 */
        /* 逐字节拼入，超长截断防溢出 */
        for (const char *q = parts[i]; *q && fl < (int)sizeof(foot) - 1; q++)
            foot[fl++] = *q;
    }
    foot[fl] = '\0';
    if (foot[0]) {
        if (cjk_text_has_wide(foot))
            cjk_text_draw_wrap(UI_MARGIN_X, UI_FOOT_TOP, max_w,
                               /*level*/0, 0, 1, foot, EPD_GFX_BLACK);
        else
            epd_gfx_draw_text(UI_MARGIN_X, UI_FOOT_BASE, foot,
                              EPD_GFX_BLACK, 1);
    }
}

/* 单列版式（全档位统一，2026-08-23 重设计）：头部单词（全宽大字
 * 自适应）+ 音标 + 收藏星标；正文流 = 释义+词根全宽分页；底部标签行 */
static void ui_draw_content(const WordEntry *w)
{
    epd_gfx_fill_rect(0, UI_STATUS_H, epd_gfx_width(),
                      epd_gfx_height() - UI_STATUS_H, EPD_GFX_WHITE);

    epd_gfx_draw_text(UI_MARGIN_X, UI_WORD_BASE, w->text, EPD_GFX_BLACK,
                      ui_fit_font(w->text, 4, UI_BODY_MAX_W));

    if (w->phonetic[0]) {
        /* 词典惯例斜杠包裹：裸 IPA 补 / /，自带包裹符（/[）不双包 */
        if (w->phonetic[0] == '/' || w->phonetic[0] == '[')
            cjk_text_draw(UI_MARGIN_X, UI_PHON_TOP, 0,
                          w->phonetic, EPD_GFX_BLACK);
        else {
            char ph[WORD_PHONETIC_MAX + 4];
            snprintf(ph, sizeof(ph), "/%s/", w->phonetic);
            cjk_text_draw(UI_MARGIN_X, UI_PHON_TOP, 0,
                          ph, EPD_GFX_BLACK);
        }
    }

    /* 收藏标记（P1）：已收藏词在音标行右缘显示 *（SET 长按切换；
     * 点阵 ASCII 与音标行同 16px 级，2026-08-23 随音标行点阵化统一） */
    if (learning_state_is_collected(study_mode_current_word_index())) {
        int sw = cjk_text_width(0, "*");
        cjk_text_draw(epd_gfx_width() - UI_MARGIN_X - sw, UI_PHON_TOP,
                      0, "*", EPD_GFX_BLACK);
    }

    /* 正文：cjk 点阵混排（中文按字断/ASCII 按词断，超宽自动换行），
     * 行数按屏高派生，超出一屏分页（多页时首行右缘页码指示）；
     * 遮蔽态走 FreeSans 英文提示 */
    if (study_mode_is_revealed())
        ui_draw_mean_paged(w);
    else
        epd_gfx_draw_text(UI_MARGIN_X, UI_BODY_TOP + (UI_MEAN_LEVEL ? 20 : 14),
                          "[SET] to reveal", EPD_GFX_BLACK, 1);

    ui_draw_foot(w, UI_BODY_MAX_W);
}

/* LAN 直传外部内容整帧直刷后调用：GFX previous 缓冲已失配，
 * 置 s_last_mode 无效值强制下一次学习界面渲染走全刷 */
extern "C" void ui_force_full_refresh_next(void)
{
    s_last_mode = MODE_COUNT;
}

/* 单词卡片渲染入口：状态机每次画面变化时调用 */
extern "C" void ui_render_word(study_mode_t mode, int index)
{
    if (wifi_config_ui_is_active()) return; /* 配置页期间不绘制学习页 */
    if (lan_server_is_active()) return;     /* LAN 接收页期间不绘制学习页 */
    if (menu_ui_is_active()) return;        /* 快捷菜单期间不绘制学习页 */

    /* 阅读模式（P3）：index=页码，渲染走 reader_engine，词库空判断
     * 不适用；实时页码由内容区页脚承担（局刷不重画状态栏） */
    if (mode == MODE_READER) {
        if (!reader_ready()) {
            epd_gfx_fill_screen(EPD_GFX_WHITE);
            reader_render_placeholder();
            epd_gfx_flush();            /* 占位页低频，一律整屏全刷 */
            s_last_mode = MODE_COUNT;
            return;
        }
        bool need_full = (mode != s_last_mode);
        if (!need_full && refresh_gfx_before_partial()) need_full = true;

        if (need_full) ui_draw_status(mode);
        epd_gfx_fill_rect(0, UI_STATUS_H, epd_gfx_width(),
                          epd_gfx_height() - UI_STATUS_H, EPD_GFX_WHITE);
        reader_render_page(index);

        if (need_full)
            epd_gfx_flush();            /* 整屏全刷 */
        else
            /* 仅局刷内容区（无窗口整屏双 RAM 差分，窗口参数仅做合法性检查） */
            epd_gfx_flush_window(0, UI_STATUS_H,
                                 epd_gfx_width(), epd_gfx_height() - UI_STATUS_H);
        s_last_mode = mode;
        return;
    }

    int total = word_parser_get_count();
    const WordEntry *w = total ? word_parser_get(index % total) : NULL;

    /* 释义页游标与词绑定：换词/换模式（含错词本进出、RST 回首、
     * 自评移词）自动归零；同词 SET 翻义保持页位 */
    if (index != s_mean_word) {
        s_mean_page = 0;
        s_mean_word = index;
    }

    if (!w) { /* 词库为空：切换到待机页（时钟/日历/天气） */
        standby_render_full();
        s_last_mode = MODE_COUNT; /* 保证日后有词时首帧全刷 */
        return;
    }

    bool need_full = (mode != s_last_mode);

    /* 残影管理：局刷达阈值时先清屏全刷（清屏后必须整屏重绘） */
    if (!need_full && refresh_gfx_before_partial()) need_full = true;

    if (need_full) {
        ui_draw_status(mode);
        ui_draw_content(w);
        epd_gfx_flush(); /* 整屏全刷 */
    } else {
        ui_draw_content(w);
        /* 仅局刷内容区（无窗口整屏双 RAM 差分，窗口参数仅做合法性检查） */
        epd_gfx_flush_window(0, UI_STATUS_H,
                             epd_gfx_width(), epd_gfx_height() - UI_STATUS_H);
    }
    s_last_mode = mode;

    LOG_I("[%s] #%d %s", study_mode_name(mode), index, w->text);
}

/* 当前应显示页面的统一渲染入口：有词库走学习页，无词库走待机页
 * （LAN/配网退出与模式切换后的恢复路径均经此路由；menu_ui 恢复退出同） */
extern "C" void ui_render_current(void)
{
    study_mode_t m = study_mode_current();
    if (m == MODE_READER)
        /* 阅读模式恢复当前页（无书显示占位页），不回待机页 */
        ui_render_word(m, study_mode_seq_pos());
    else if (word_parser_get_count() > 0)
        ui_render_word(m, 0);
    else
        standby_render_full();
}

/* P5 幻影按键吞除武装标志：按键唤醒的会话置位（setup），on_button 吞掉
 * 唤醒后首个中键事件后清位（见 on_button 顶部注释） */
static bool s_wake_swallow_center = false;

/* 五向导航按键事件回调 */
static void on_button(nav_key_t id, button_event_t event)
{
    /* P5：任何按键事件都刷新无操作计时（含配网/门户转发与退出路径） */
    power_note_activity();

    /* P5：深睡唤醒幻影事件吞除：唤醒键（中键）按住唤醒时，扫描任务
     * 零状态起步，释放时误报 SHORT（中键=发音）或按住超阈值误报 LONG
     * （中键=进配网）；吞掉唤醒后首个中键事件（真实按压最多迟一次
     * 发音，无破坏性；唤醒确认 20ms 震动已在 setup 给出） */
    if (s_wake_swallow_center && id == NAV_CENTER) {
        s_wake_swallow_center = false;
        return;
    }

    /* 快捷菜单激活时，按键全部转发（顶层覆盖层，与配网页同级语义） */
    if (menu_ui_is_active()) {
        menu_ui_on_button(id, event);
        return;
    }

    /* Wi-Fi 配置页激活时，按键全部转发 */
    if (wifi_config_ui_is_active()) {
        wifi_config_ui_on_button(id, event);
        return;
    }

    /* LAN 接收页 / AP portal 激活时，任意按键退出并回到学习界面
     * （portal 模式下 lan_portal_exit 关热点回 STA；均为幂等调用） */
    if (lan_server_is_active()) {
        lan_portal_exit();
        lan_server_leave_receive_page();
        ui_render_current();
        return;
    }

    /* 待机页激活时（词库为空），按键交给待机页处理 */
    if (standby_is_active()) {
        standby_on_button(id, event);
        return;
    }

    /* 长按功能集中在五键上：中=功能菜单，上=清残影，下=模式切换，
     * 左=AP 门户（隔离环境下 STA 页面不可达时的可靠通道），
     * 右=LAN 接收页 */
    if (event == BUTTON_EVENT_LONG_PRESS) {
        switch (id) {
        case NAV_CENTER:
            menu_ui_enter();
            return;
        case NAV_UP:
            LOG_I("user requested ghost-clear full refresh");
            refresh_force_full();
            return;
        case NAV_DOWN:
            /* 切换学习模式并重绘（ui_render_word 检测到模式变化自动全刷） */
            haptic_event(HAPTIC_MODE);   /* 模式切换 50ms（PRD 5.4） */
            study_mode_switch_next();
            ui_render_current();
            return;
        case NAV_LEFT:
            lan_portal_enter();
            return;
        case NAV_RIGHT:
            /* 幂等启动服务器并显示访问 URL */
            lan_server_enter_receive_page();
            return;
        case NAV_SET:
            /* 收藏/取消当前词（P1）：局部重绘内容区刷新 * 标记；
             * 阅读模式无“当前词”概念，不响应；
             * 收藏视图（MODE_COLLECTION）内=移出序列（after_uncollect
             * 收缩钳位，清空自动退回闪卡），2026-08-23 */
            if (study_mode_current() == MODE_READER) return;
            haptic_event(HAPTIC_REVIEW); /* 确认型操作归自评档 30ms（PRD 5.4 未单列） */
            learning_state_toggle_collect(study_mode_current_word_index());
            if (study_mode_current() == MODE_COLLECTION &&
                study_mode_after_uncollect()) {
                ui_render_current();   /* 清空退回闪卡或游标收缩，重绘当前页 */
                return;
            }
            ui_render_word(study_mode_current(),
                           study_mode_current_word_index());
            return;
        case NAV_RST:
            /* 临时视图进出三级判：错词本/收藏浏览内=退出，否则进错词本
             * （无错词 100ms 长震边界反馈，PRD 5.4） */
            if (study_mode_current() == MODE_WRONGBOOK) {
                study_mode_exit_wrongbook();
            } else if (study_mode_current() == MODE_COLLECTION) {
                study_mode_exit_collection();
            } else if (!study_mode_enter_wrongbook()) {
                haptic_event(HAPTIC_ERROR);
                return;
            }
            haptic_event(HAPTIC_MODE);
            ui_render_current(); /* 模式变化 -> 全刷重绘第一条 */
            return;
        default:
            return;
        }
    }

    /* 短按：上/下翻词（释义多页时先词内翻释义页），中=发音，
     * SET=遮蔽/揭晓释义，RST=回第一条；
     * 左=自评「忘记」Q1，右=自评「简单」Q5（SM-2 评分入 learning_state，
     * 错词本内答对自动移出，序列清空自动退回闪卡） */
    if (event != BUTTON_EVENT_SHORT_PRESS) return;

    /* 阅读模式短按路由（P3）：上/下=翻页，左/右=字号缩放，
     * RST=回第一页（“回到当前模式第一条”全局语义）；
     * 词相关动作（发音/自评/遮蔽）不适用，中/SET 忽略 */
    if (study_mode_current() == MODE_READER) {
        switch (id) {
        case NAV_UP:
            study_mode_handle_action(0);      /* 上一页 */
            return;
        case NAV_DOWN:
            study_mode_handle_action(1);      /* 下一页 */
            return;
        case NAV_LEFT:
            study_mode_reader_font_step(-1);  /* 字号缩小（震动由去抖层 20ms 覆盖） */
            return;
        case NAV_RIGHT:
            study_mode_reader_font_step(+1);  /* 字号放大 */
            return;
        case NAV_RST:
            study_mode_reset_cursor();        /* 回第一页 */
            return;
        default:
            return;
        }
    }

    switch (id) {
    case NAV_UP:
        if (ui_mean_page_step(-1)) return;  /* 释义多页：词内上一页 */
        study_mode_handle_action(0);   /* prev */
        return;
    case NAV_DOWN:
        if (ui_mean_page_step(+1)) return;  /* 释义多页：词内下一页 */
        study_mode_handle_action(1);   /* next */
        return;
    case NAV_CENTER:
        study_mode_handle_action(3);   /* speak */
        return;
    case NAV_SET:
        study_mode_handle_action(2);   /* confirm：遮蔽/揭晓释义 */
        return;
    case NAV_RST:
        study_mode_reset_cursor();     /* 回到当前模式第一条 */
        return;
    case NAV_LEFT:
        learning_state_apply_quality(study_mode_current_word_index(), 1);
        haptic_event(HAPTIC_REVIEW);   /* 自评提交 30ms（PRD 5.4） */
        if (study_mode_after_quality(1))
            ui_render_word(study_mode_current(),
                           study_mode_current_word_index());
        return;
    case NAV_RIGHT:
        learning_state_apply_quality(study_mode_current_word_index(), 5);
        haptic_event(HAPTIC_REVIEW);   /* 自评提交 30ms（PRD 5.4） */
        if (study_mode_after_quality(5))
            ui_render_word(study_mode_current(),
                           study_mode_current_word_index());
        return;
    default:
        return;
    }
}

/* 后台心跳 + OTA 检查任务。
 * 启动阶段：每 2s 轮询，联网即立即启动 LAN 直传服务（不设上限：
 *           即使路由器后启动/断电恢复，联网后也能尽快拉起服务）；
 * 之后转为 10 分钟周期：上报队列 flush + 首次注册 + 心跳 + OTA
 * 检查（含服务兜底重启，幂等） */
/* ============================================================
 * 云端同步凭据与上报 flush (P2)
 * 凭据链：NVS "inkword"/{api_url, dev_key} → sync_set_*；无 key 时
 * 联网后按 MAC 幂等注册（后端返回既有 ApiKey）并回写 NVS。
 * ============================================================ */

static void sync_credentials_load(void)
{
    char url[128], key[64];
    nvs_handle_t h;
    if (nvs_open("inkword", NVS_READONLY, &h) != ESP_OK) {
        sync_set_base_url(INKWORD_API_BASE);
        return;
    }
    size_t len = sizeof(url);
    if (nvs_get_str(h, "api_url", url, &len) == ESP_OK)
        sync_set_base_url(url);
    else
        sync_set_base_url(INKWORD_API_BASE);
    len = sizeof(key);
    if (nvs_get_str(h, "dev_key", key, &len) == ESP_OK)
        sync_set_device_key(key);
    nvs_close(h);
}

static void sync_try_register(void)
{
    if (sync_has_device_key()) return;

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[13];
    snprintf(mac_str, sizeof(mac_str), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    char key[64];
    if (sync_register(mac_str, NULL, key, sizeof(key)) == 0) {
        sync_set_device_key(key);
        nvs_handle_t h;
        if (nvs_open("inkword", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, "dev_key", key);
            nvs_commit(h);
            nvs_close(h);
        }
        LOG_I("device registered, key persisted");
    } else {
        LOG_W("register failed, retry next cycle");
    }
}

/* 上报队列 flush：逐条发送（人手按键频次下 HTTP 开销可忽略；攒批优化
 * 待设备规模上来后）。无 cloudId 的词（本地导入）直接丢弃；任一条
 * 失败即停，队列保留待下周期重试（timestamp=0 由服务器落地时间代替） */
static void sync_flush_pending(void)
{
    int guard = learning_state_event_count();
    while (guard-- > 0) {
        lr_event_t ev;
        if (!learning_state_event_peek(0, &ev)) break;

        const WordEntry *w = word_parser_get(ev.word_idx);
        if (!w || !w->cloud_id[0]) {
            learning_state_event_drop(1); /* 本地词：无云端身份，事件无价值 */
            continue;
        }

        if (ev.quality >= 0) {
            ProgressItem it = {};   /* 全零初始化（quality/word_id/timestamp） */
            it.quality = (uint8_t)ev.quality;
            it.timestamp = 0;
            strncpy(it.word_id, w->cloud_id, sizeof(it.word_id) - 1);
            if (sync_push_progress(&it, 1) != 0) return;
        } else {
            if (sync_push_collect(w->cloud_id, ev.collected) != 0) return;
        }
        learning_state_event_drop(1);
    }
}

/* ============================================================
 * P5 静默心跳会话：RTC TIMER 唤醒后的极简启动路径（不返回）
 * 屏/SD/音频/学习状态全不初始化：墨水屏驻留末帧不碰 COG，
 * sync_flush_pending 的 guard=learning_state_event_count()=0（静态
 * 零初始化）自然空转——事件队列是内存态且仅由按键产生，入睡时已
 * 论证必空（见 power_enter_sleep 注释）。NVS 必须初始化（凭据/时钟
 * checkpoint 均在 NVS）。业务链：Wi-Fi 快连（10s 超时失败静默回睡，
 * 不重试不闪屏）→ HTTP Date 校时（standby_page 静态基准对无需
 * standby_init 即可写）→ 注册/上报/心跳/OTA 检查 → 回睡。
 * ============================================================ */
static void silent_heartbeat_session(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    wifi_manager_init();
    sync_credentials_load();

    /* 未配网的新设备（无凭据即入睡）：不白等超时直接回睡 */
    if (!wifi_has_saved_credentials()) {
        LOG_W("silent session: no wifi credentials, back to sleep");
        power_enter_sleep(PM_HEARTBEAT_PERIOD_S);
    }

    /* 连接为事件驱动异步（wifi_manager_init 内自动连已存网络），
     * 轮询等待：路由器在线典型 2~3s，离线等满 10s 静默回睡 */
    int waited_s = 0;
    while (!wifi_is_connected() && waited_s < PM_WAKE_WIFI_TIMEOUT_S) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        waited_s++;
    }
    if (!wifi_is_connected()) {
        LOG_W("silent session: wifi timeout (%ds), back to sleep", waited_s);
        power_enter_sleep(PM_HEARTBEAT_PERIOD_S);
    }

    /* HTTP Date 校时：刷新 standby 自治钟内存基准（回睡前由
     * power_enter_sleep 内 checkpoint 落 NVS，下级唤醒用新基准） */
    int64_t now = sync_fetch_http_time();
    if (now > 0) {
        standby_time_set(now);
        LOG_I("silent session: clock calibrated (epoch=%lld)", (long long)now);
    }

    /* 云端闭环（与 background_task 周期段同链）：幂等注册 + 上报 flush */
    sync_try_register();
    sync_flush_pending();
    int bat = 100; /* TODO: 读取 ADC 电量（与 background_task 同占位） */
    sync_heartbeat(bat, FW_VERSION);

    /* OTA 检查：升级成功即重启进新固件（走正常启动路径 ota_mark_valid） */
    char url[256], md5[64];
    int size = 0;
    if (ota_check_for_update(url, sizeof(url), md5, sizeof(md5), &size)) {
        LOG_I("OTA update found (silent session), size=%d", size);
        ota_perform_upgrade(url, md5);
    }

    power_enter_sleep(PM_HEARTBEAT_PERIOD_S);   /* 不返回 */
}

static void background_task(void *arg)
{
    (void)arg;
    while (!lan_server_is_running()) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (wifi_is_connected()) {
            lan_server_start(); /* 幂等 */
        }
    }
    const TickType_t period = pdMS_TO_TICKS(10 * 60 * 1000); /* 10 分钟 */
    int wx_poll_cnt = 2; /* 待机页天气轮询计数：初始 2 -> 首个周期即拉取 */
    while (1) {
        vTaskDelay(period);
        if (wifi_is_connected()) {
            lan_server_start(); /* 兜底：服务异常停止则重启（幂等） */

            /* 云端闭环（P2）：首次注册（幂等）+ 评分/收藏上报 flush */
            sync_try_register();
            sync_flush_pending();

            int bat = 100; /* TODO: 读取 ADC 电量 */
            sync_heartbeat(bat, FW_VERSION);

            /* 待机页天气：每 3 个周期（约 30 分钟）拉取一次，失败下周期重试；
             * 仅待机页激活时拉取（学习页不耗流量） */
            if (standby_is_active() && ++wx_poll_cnt >= 3) {
                weather_info_t wx;
                if (sync_fetch_weather(&wx) == 0) {
                    standby_weather_update(&wx);
                    wx_poll_cnt = 0;
                } else {
                    wx_poll_cnt = 2;
                }
            }

            /* 顺带检查 OTA */
            char url[256], md5[64];
            int size = 0;
            if (ota_check_for_update(url, sizeof(url), md5, sizeof(md5), &size)) {
                LOG_I("OTA update found, size=%d", size);
                /* 自动升级可改为需用户确认 */
                ota_perform_upgrade(url, md5);
            }
        }
    }
}

/* Arduino setup - 初始化所有组件 */
void setup()
{
    Serial.begin(115200);
    delay(100);

    /* 1. 日志与 NVS */
    log_init();
    LOG_I("=== InkWord firmware %s booting ===", FW_VERSION);

    /* 1.5 P5 电源分流（先于一切外设）：TIMER 唤醒 = 静默心跳会话
     *     （校时/上报/OTA 后回睡，不返回）；中键唤醒/冷启动走下方
     *     正常流程。gpio_hold 跨深睡锁存的 EPD 引脚已在 power_init 释放 */
    if (power_init() == ESP_SLEEP_WAKEUP_TIMER)
        silent_heartbeat_session();
    if (power_woke_by_button())
        s_wake_swallow_center = true;  /* 唤醒键幻影事件吞除武装 */

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* 2. 存储 / 屏幕 / 音频 / 按键 */
    if (storage_init() == 0) {
        storage_list_dir(SD_MOUNT_POINT);
    } else {
        LOG_W("SD card init failed, running without word DB");
    }

    epd_driver_init();
    epd_clear_screen();                 /* 显示启动白屏 */
    power_mark_periph_online();         /* P5：本会话外设在线（入睡时收口外设） */

    audio_init();
    haptic_init();                   /* 触觉反馈（P2 震动）：先于按键扫描任务 */
    if (power_woke_by_button())
        haptic_event(HAPTIC_KEYPRESS); /* P5：唤醒确认 20ms（先于屏恢复完成） */
    button_handler_init();
    button_register_callback(on_button);

    /* 3. 刷新调度器：学习/阅读页局刷阈值=8（2026-08-20 无窗口方案定稿：
     * 双 RAM 差分局刷自身无残影，全刷降为低频深度保养，N=8 平衡闪烁
     * 频率；待机页走独立 _n 阈值 12，见 standby_page.c） */
    refresh_scheduler_init(8);

    /* 4. WiFi 联网（失败不阻塞主流程）；同步凭据（base URL / 设备 key）
     *    从 NVS 恢复到 sync_client，首次注册留待联网后 background_task */
    wifi_manager_init();
    sync_credentials_load();

    /* 4.5 Wi-Fi 配置：无凭据时自动开启 AP 配网门户
     *     （手机连 InkWord-Setup 热点后自动弹出配置页）；
     *     软键盘配置 UI 仍可长按 C 进入 */
    wifi_config_ui_init();
    if (!wifi_has_saved_credentials()) {
        LOG_W("no saved WiFi, starting AP portal");
        lan_portal_enter();
    }

    /* 4.6 BLE 配网服务（App 扫描发现/配网；失败仅告警，
     *     Portal 与软键盘配网路径不受影响）。
     *     默认禁用，根因见文件头 INKWORD_BLE_PROVISION 注释 */
#if INKWORD_BLE_PROVISION
    ble_provision_init();
#endif

    /* 5. 标记当前固件有效，防止 OTA 回滚 */
    ota_mark_valid();

    /* 6. 加载词库（词池 PSRAM 化，2026-08-20）：按 MAX_WORDS 逐半降级
     *    分配，与阅读器书缓冲共享 8MB Octal；全部分配失败（极小概率）
     *    置空容量，词库空走待机页 */
    for (int cap = MAX_WORDS; cap > 0 && !s_word_pool; cap /= 2) {
        s_word_pool = (WordEntry *)heap_caps_malloc(
            (size_t)cap * sizeof(WordEntry), MALLOC_CAP_SPIRAM);
        if (s_word_pool) s_word_cap = cap;
        else LOG_W("word pool alloc %d entries failed, halving", cap);
    }
    LOG_I("word pool: %d entries x %uB = %uKB PSRAM",
          s_word_cap, (unsigned)sizeof(WordEntry),
          (unsigned)((size_t)s_word_cap * sizeof(WordEntry) / 1024));

    const char *word_file = SD_MOUNT_POINT "/words.json";
    if (s_word_pool && storage_file_exists(word_file)) {
        int n = word_parser_load(word_file, s_word_pool, s_word_cap);
        LOG_I("word DB loaded: %d entries", n);
    } else if (s_word_pool) {
        /* 出厂内嵌兜底（2026-08-23）：无 SD 卡开箱即用。词库随固件烧入
         * rodata（platformio.ini embed_files，生成链见
         * tools/default_vocab），load_mem 零拷贝直吃。SD 词库存在时仍
         * 优先（可更新、可携带 cloudId）；内嵌版本无 cloudId（本地词条，
         * 评分/收藏不上报），在线同步/导出路径下发的词库才携带 */
        extern const uint8_t _binary_src_default_words_json_start[];
        extern const uint8_t _binary_src_default_words_json_end[];
        size_t len = (size_t)(_binary_src_default_words_json_end -
                              _binary_src_default_words_json_start);
        int n = word_parser_load_mem(
            (const char *)_binary_src_default_words_json_start, len,
            s_word_pool, s_word_cap);
        LOG_I("embedded word DB loaded: %d entries (%u bytes)",
              n, (unsigned)len);
    } else {
        LOG_W("words.json not found on SD card");
    }
#if INKWORD_DEMO_WORDS
    /* 测试构建：内嵌词库也被排除时（如裁剪验证）的最后一道演示词 */
    if (s_word_pool && word_parser_get_count() == 0) {
        word_parser_load_demo(s_word_pool, s_word_cap);
    }
#endif

    /* 6.5 本地学习状态（P1 错词本/收藏）：按词库规模锁定并从 NVS 恢复；
     *     必须先于 study_mode_init/首次渲染（错词序列与收藏标记依赖） */
    learning_state_init(word_parser_get_count());

    /* 6.9 P5 深睡唤醒时钟恢复：自治钟基准对经 RTC 慢钟差分重建
     *     （仅按键唤醒路径；冷启动/复位 cause=UNDEFINED 不走此路，
     *     维持未同步留白等 HTTP 校准。须在待机页首渲染/tick 之前） */
    if (power_woke_by_button())
        standby_time_restore();

    /* 7. 初始化待机页（恢复 NVS 天气缓存），进入上次学习模式；
     *    无词库时渲染待机页（时钟/日历/天气） */
    standby_init();
    /* 7.5 阅读引擎（P3）：找书整本入 PSRAM + 建页表；必须先于
     *     study_mode_init（READER 模式恢复阅读页进度/页数依赖页表） */
    reader_engine_init();
    study_mode_init();
    if (word_parser_get_count() > 0 && study_mode_current() != MODE_READER) {
        study_mode_handle_action(1);    /* 渲染第一条 */
    } else {
        ui_render_current();            /* READER 书页/占位页 或 待机页 */
    }

    /* 8. 启动后台任务（心跳/OTA） */
    xTaskCreate(background_task, "bg", 6 * 1024, NULL, 4, NULL);

    LOG_I("=== InkWord ready ===");
}

/* Arduino loop - 主循环（事件驱动；待机页分钟级心跳由 standby_tick 承载，
 * 非待机状态时零开销返回；学习状态脏标记静默 5s 后在非按键路径落盘） */
void loop()
{
    standby_tick();
    learning_state_maybe_save();  /* LR02 sparse 延迟保存（无脏零开销） */
    power_maybe_sleep();          /* P5：无操作超时且无禁睡条件则入睡（不返回） */
    vTaskDelay(pdMS_TO_TICKS(1000));
}
