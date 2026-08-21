/**
 * @file main.cpp
 * @brief 应用主入口 (Task F-20)
 *
 * 启动流程：日志 -> NVS -> 存储(SD) -> 屏幕 -> 音频 -> 按键 ->
 *          WiFi -> 词库 -> 进入上次模式 -> 主循环（事件驱动）。
 *
 * 五向导航按键映射（2026-08 取代 6 独立按键；SET/RST 侧键同月接入）：
 *   上 短按=上一条 / 长按=清残影全刷；
 *   下 短按=下一条 / 长按=切换学习模式；
 *   中 短按=发音 / 长按=进入 Wi-Fi 配置；
 *   左 短按=自评「忘记」Q1（SM-2 质量分 1：连错+1，>0 入错词本）/ 长按=进入 AP 直连/配网门户
 *        （手机连 InkWord-Setup 热点直传，绕开路由器隔离；任意键退出）；
 *   右 短按=自评「简单」Q5（SM-2 质量分 5：连错清零，错词本中移出）/ 长按=进入 LAN 接收页（同网浏览器直传，任意键退出）；
 *   SET 短按=遮蔽/揭晓释义（闪卡自测；待机页=轮换下一条引文）/ 长按=收藏/取消当前词（左栏 * 标记）；
 *   RST 短按=回到当前模式第一条 / 长按=错词本进出（连错>0 过滤视图，答对清空自动退出）。
 *
 * 阅读模式（P3，长按下循环切换进入）：上/下=翻页，左/右=字号缩放
 * （16/20/24px 三级循环，按当前页首字符就近保持阅读位置），RST=回
 * 第一页；中/SET 短按与词相关长按（收藏）不适用；进度自动保存
 * （NVS rd_*）。
 *
 * 无词库待机页（词库为空时默认显示，见 standby_page.c）：
 *   时钟/日历/天气整页；待机态长按语义与学习页一致（配网/清残影/门户/LAN），
 *   短按中=立即拉取天气，其余短按忽略。
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
#include "srs_engine.h"
#include "learning_state.h"
#include "study_mode_machine.h"
#include "wifi_manager.h"
#include "wifi_config_ui.h"
#include "sync_client.h"
#include "ota_manager.h"
#include "lan_display_server.h"
#include "standby_page.h"
#include "reader_engine.h"   /* 阅读模式（P3）：书分页/字号/进度 */
#include "ble_provision.h"

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
#define INKWORD_API_BASE "https://api.inkword.example.com"
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
 * 布局（GFX 横屏 416x240，rotation=1；FreeSans 基线 y / 点阵顶左 y）：
 *   y[0,32)    状态栏：模式名（左）/ 序号（右）/ 分隔线
 *   左栏 x[16,248)  单词(24pt 超宽自动降级) + 音标(9pt) + 底部标签
 *                  （tag 含中文时走 16px 点阵，见 cjk_text）
 *   竖分隔线 x=248；右栏 x[264,400) 释义(16px 点阵混排自动断行 ≤6 行，
 *                  中文释义可渲染——真实词库释义为中文，FreeSans 仅 ASCII)
 *
 * 刷新策略（2026-08-20 无窗口方案定稿，见 README「局部刷新方案」）：
 *   - 首帧 / 模式切换 / 保养：整屏重绘 + 全刷（epd_gfx_flush）
 *   - 同模式翻词/翻页：重绘内容区 + 局刷（epd_gfx_flush_window，
 *     无窗口整屏双 RAM 差分：整屏写 0x10 旧帧 + 0x13 新帧，COG
 *     全屏差分只翻转变化像素，无闪烁；状态栏不重绘自动跳过）
 *   - 残影管理：局刷计数达阈值（学习/阅读页 8 次）时升级为整屏重绘
 *     + 真全刷低频保养（局刷自身无残影，全刷仅防累积）
 * ============================================================ */

#define UI_STATUS_H     32    /* 状态栏高度（内容区顶 y；无窗口差分下不再要求 8 对齐） */
#define UI_MARGIN_X     16    /* 左右留白 */
#define UI_STATUS_BASE  22    /* 状态栏文字基线 y */
#define UI_WORD_BASE    100   /* 单词基线 y（左栏，24pt 超宽自动降级） */
#define UI_PHON_BASE    132   /* 音标基线 y（左栏，9pt） */
#define UI_VSEP_X       248   /* 左右分栏竖线 x */
#define UI_MEAN_X       264   /* 释义起始 x（右栏） */
#define UI_MEAN_TOP     48    /* 释义首行顶 y（右栏，16px 点阵顶左语义） */
#define UI_MEAN_LH      26    /* 释义行距 */
#define UI_MEAN_LINES   6     /* 释义最大行数（超出截断） */
#define UI_MEAN_MAX_W   (EPD_GFX_WIDTH - UI_MEAN_X - UI_MARGIN_X) /* 右栏文本宽 */
#define UI_WORD_MAX_W   (UI_VSEP_X - 2 * UI_MARGIN_X)             /* 左栏文本宽 */
#define UI_FOOT_BASE    224   /* 左栏底部标签基线 y（9pt，纯 ASCII tag） */
#define UI_FOOT_TOP     206   /* 中文 tag 16px 点阵顶 y（UI_FOOT_BASE-16-2） */
#define UI_MEAN_HINT_BASE 62  /* 遮蔽态 FreeSans 提示基线（沿用旧释义基线） */
#define UI_ROOT_TOP     156   /* 词根行顶 y（左栏，音标下空白区；≤2 行） */
#define UI_ROOT_LINES   2     /* 词根行数上限（超出截断） */
#define UI_ROOT_LH      20    /* 词根行距（16px 字 + 4 间距） */

static study_mode_t s_last_mode = MODE_COUNT; /* 无效值：首帧强制全刷 */

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

/* 绘制状态栏：模式名（左）+ 序号（右，错词本=序号/错词数）+ 分隔线 */
static void ui_draw_status(study_mode_t mode)
{
    epd_gfx_fill_rect(0, 0, EPD_GFX_WIDTH, UI_STATUS_H, EPD_GFX_WHITE);

    epd_gfx_draw_text(UI_MARGIN_X, UI_STATUS_BASE,
                      study_mode_name(mode), EPD_GFX_BLACK, 1);

    char buf[24];
    int total = study_mode_seq_total();
    int tw, th;
    snprintf(buf, sizeof(buf), "%d/%d",
             total ? study_mode_seq_pos() + 1 : 0, total);
    epd_gfx_text_bounds(buf, 1, &tw, &th);
    epd_gfx_draw_text(EPD_GFX_WIDTH - UI_MARGIN_X - tw, UI_STATUS_BASE,
                      buf, EPD_GFX_BLACK, 1);

    epd_gfx_draw_hline(UI_MARGIN_X, UI_STATUS_H,
                       EPD_GFX_WIDTH - 2 * UI_MARGIN_X, EPD_GFX_BLACK);
}

/* 绘制内容区：左栏单词卡片 + 竖分隔线 + 右栏释义 */
static void ui_draw_content(const WordEntry *w)
{
    epd_gfx_fill_rect(0, UI_STATUS_H, EPD_GFX_WIDTH,
                      EPD_GFX_HEIGHT - UI_STATUS_H, EPD_GFX_WHITE);

    epd_gfx_draw_text(UI_MARGIN_X, UI_WORD_BASE, w->text, EPD_GFX_BLACK,
                      ui_fit_font(w->text, 4, UI_WORD_MAX_W));

    if (w->phonetic[0])
        epd_gfx_draw_text(UI_MARGIN_X, UI_PHON_BASE,
                          w->phonetic, EPD_GFX_BLACK, 1);

    /* 收藏标记（P1）：已收藏词在左栏音标行右侧显示 *（SET 长按切换） */
    if (learning_state_is_collected(study_mode_current_word_index())) {
        int sw, sh;
        epd_gfx_text_bounds("*", 2, &sw, &sh);
        epd_gfx_draw_text(UI_VSEP_X - UI_MARGIN_X - sw, UI_PHON_BASE,
                          "*", EPD_GFX_BLACK, 2);
    }

    epd_gfx_draw_vline(UI_VSEP_X, UI_STATUS_H + 16,
                       EPD_GFX_HEIGHT - UI_STATUS_H - 32, EPD_GFX_BLACK);

    /* 释义：16px 点阵混排（中文按字断/ASCII 按词断，超宽自动换行，
     * 超 6 行截断）；遮蔽态仍走 FreeSans 英文提示 */
    if (study_mode_is_revealed()) {
        cjk_text_draw_wrap(UI_MEAN_X, UI_MEAN_TOP, UI_MEAN_MAX_W,
                           /*level*/0, UI_MEAN_LH, UI_MEAN_LINES,
                           w->meaning, EPD_GFX_BLACK);
    } else {
        /* 遮蔽自测态：右栏仅提示，不画释义（SET 揭晓） */
        epd_gfx_draw_text(UI_MEAN_X, UI_MEAN_HINT_BASE,
                          "[SET] to reveal", EPD_GFX_BLACK, 1);
    }

    /* 词根行（左栏音标下空白区）：root 非空才画，≤2 行截断；
     * 词根本文自带 “=” 语义不加前缀（与音标裸文本一致） */
    if (w->root[0])
        cjk_text_draw_wrap(UI_MARGIN_X, UI_ROOT_TOP, UI_WORD_MAX_W,
                           /*level*/0, UI_ROOT_LH, UI_ROOT_LINES,
                           w->root, EPD_GFX_BLACK);

    /* 底部标签行：tag·grade·source 非空项以间隔号拼接（间隔号在
     * 字库全角标点集内）；含中文走 16px 点阵单行截断，纯 ASCII
     * 且无扩展字段时保持 FreeSans 9pt */
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
            cjk_text_draw_wrap(UI_MARGIN_X, UI_FOOT_TOP, UI_WORD_MAX_W,
                               /*level*/0, 0, 1, foot, EPD_GFX_BLACK);
        else
            epd_gfx_draw_text(UI_MARGIN_X, UI_FOOT_BASE, foot,
                              EPD_GFX_BLACK, 1);
    }
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
        epd_gfx_fill_rect(0, UI_STATUS_H, EPD_GFX_WIDTH,
                          EPD_GFX_HEIGHT - UI_STATUS_H, EPD_GFX_WHITE);
        reader_render_page(index);

        if (need_full)
            epd_gfx_flush();            /* 整屏全刷 */
        else
            /* 仅局刷内容区（无窗口整屏双 RAM 差分，窗口参数仅做合法性检查） */
            epd_gfx_flush_window(0, UI_STATUS_H,
                                 EPD_GFX_WIDTH, EPD_GFX_HEIGHT - UI_STATUS_H);
        s_last_mode = mode;
        return;
    }

    int total = word_parser_get_count();
    const WordEntry *w = total ? word_parser_get(index % total) : NULL;

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
                             EPD_GFX_WIDTH, EPD_GFX_HEIGHT - UI_STATUS_H);
    }
    s_last_mode = mode;

    LOG_I("[%s] #%d %s", study_mode_name(mode), index, w->text);
}

/* 当前应显示页面的统一渲染入口：有词库走学习页，无词库走待机页
 * （LAN/配网退出与模式切换后的恢复路径均经此路由） */
static void ui_render_current(void)
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

/* 五向导航按键事件回调 */
static void on_button(nav_key_t id, button_event_t event)
{
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

    /* 长按功能集中在五键上：中=配网，上=清残影，下=模式切换，
     * 左=AP 门户（隔离环境下 STA 页面不可达时的可靠通道），
     * 右=LAN 接收页 */
    if (event == BUTTON_EVENT_LONG_PRESS) {
        switch (id) {
        case NAV_CENTER:
            wifi_config_ui_enter();
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
             * 阅读模式无“当前词”概念，不响应 */
            if (study_mode_current() == MODE_READER) return;
            haptic_event(HAPTIC_REVIEW); /* 确认型操作归自评档 30ms（PRD 5.4 未单列） */
            learning_state_toggle_collect(study_mode_current_word_index());
            ui_render_word(study_mode_current(),
                           study_mode_current_word_index());
            return;
        case NAV_RST:
            /* 错词本进出（P1）：无错词 100ms 长震边界反馈（PRD 5.4） */
            if (study_mode_current() == MODE_WRONGBOOK) {
                study_mode_exit_wrongbook();
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

    /* 短按：上/下翻词，中=发音，SET=遮蔽/揭晓释义，RST=回第一条；
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
        study_mode_handle_action(0);   /* prev */
        return;
    case NAV_DOWN:
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

    audio_init();
    haptic_init();                   /* 触觉反馈（P2 震动）：先于按键扫描任务 */
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
    } else {
        LOG_W("words.json not found on SD card");
    }
#if INKWORD_DEMO_WORDS
    /* 测试构建：无 SD 词库时内嵌演示词，验证学习页按键 */
    if (s_word_pool && word_parser_get_count() == 0) {
        word_parser_load_demo(s_word_pool, s_word_cap);
    }
#endif

    /* 6.5 本地学习状态（P1 错词本/收藏）：按词库规模锁定并从 NVS 恢复；
     *     必须先于 study_mode_init/首次渲染（错词序列与收藏标记依赖） */
    learning_state_init(word_parser_get_count());

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
    vTaskDelay(pdMS_TO_TICKS(1000));
}
