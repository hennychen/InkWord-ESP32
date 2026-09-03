/**
 * @file chat_ui.c
 * @brief AI 对话屏显（P2B 建块 / T1.3 自 main.cpp 迁出，修 A1）
 *
 * 迁移口径（T1.3，2026）：函数体零改动，函数名沿用迁出前
 * （ui_render_chat / ui_chat_anim_tick）；布局宏自 main.cpp 同步
 * 复制（值与派生式逐字节一致；T1.5 参数表落地时一并收敛）。
 *
 * P2B AI 对话屏显（chat_mode 任务驱动；状态区局刷同 ui_render_pron
 * 策略，环路内禁全刷红线。语音优先、屏幕克制：仅状态词 + 末句回复
 * ≤2 行（听不清时看屏）。三色面板 partial_enabled=false 零渲染，
 * 纯语音+震动（与待机页轮换停用同款 UX 降级先例）
 * ===== AI 对话 Siri 球（聆听/思考状态页中央图标，2026-09-01） =====
 * 版式仅 MID+ 档（TINY/SMALL 内容区高度不足，保持纯文字版式同先例
 * 降级）；动画仅 THINKING 期（读流循环 ui_chat_anim_tick 驱动涟漪
 * ~2fps）；RECORDING 期零刷屏——I2S RX DMA 缓冲 128ms，任何局刷
 * 阻塞都会丢样本（音频保真红线），静态球页在 adc_start 前刷就 */
#include "chat_ui.h"

#include <stdio.h>

#include "debug_log.h"
#include "epd_driver.h"           /* epd_gfx_* */
#include "cjk_text.h"             /* cjk_text_* */
#include "layout_profile.h"       /* layout_profile_get/LAYOUT_MID */
#include "settings_ui.h"          /* settings_font_mode（UI_MEAN_LEVEL） */
#include "page_router.h"         /* T2.2 守卫统一：display_busy（P2 注册制） */

#include <esp_timer.h>            /* esp_timer_get_time（anim_tick 节拍） */

static const char *TAG = "CHAT_UI";   /* debug_log.h LOG 宏依赖 */

/* ---- 布局宏（T1.5 参数表收敛完成：布局值查 layout_profile 字段，
 * 字号/行距派生式局部保留；语义同 main.cpp 学习页布局宏区）---- */
#define UI_TINY         (layout_profile_get()->kind == LAYOUT_TINY)
#define UI_STATUS_H     (layout_profile_get()->status_h)  /* 状态栏高度（T1.5） */
#define UI_MARGIN_X     (layout_profile_get()->margin_x)   /* 左右留白（T1.5） */
#define UI_MEAN_LEVEL   (layout_profile_get()->kind <= LAYOUT_SMALL \
                         ? (settings_font_mode() >= 1 ? 1 \
                            : (layout_profile_get()->narrow_tiny ? 1 : 0)) \
                         : (settings_font_mode() >= 1 ? 2 : 1))
#define UI_WORD_BASE    (UI_STATUS_H + (UI_TINY ? 24 \
                         : (UI_MEAN_LEVEL ? 36 : 32)))  /* 单词基线 */
#define UI_PHON_TOP     (UI_WORD_BASE + (UI_TINY ? 6 : 9)) /* 音标行顶 */
#define UI_AUX_LEVEL    (layout_profile_get()->narrow_tiny ? UI_MEAN_LEVEL : 0)  /* 辅助字级（T1.5 档位化） */
#define UI_BODY_TOP     (UI_PHON_TOP + (UI_AUX_LEVEL ? 20 : 16) + (UI_TINY ? 4 : 11))
#define UI_BODY_LH      (UI_TINY ? (UI_MEAN_LEVEL ? 26 : 20) : (UI_MEAN_LEVEL ? 24 : 20))
#define UI_BODY_MAX_W   (epd_gfx_width() - 2 * UI_MARGIN_X)

static int ui_chat_isqrt(int v)
{
    int r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}

/* 行扫描实心圆（GFX 无圆 API，r<=30 场景微秒级） */
static void ui_chat_fill_circle(int cx, int cy, int r, uint16_t color)
{
    for (int dy = -r; dy <= r; dy++) {
        int dx = ui_chat_isqrt(r * r - dy * dy);
        epd_gfx_fill_rect(cx - dx, cy + dy, 2 * dx + 1, 1, color);
    }
}

#define CHAT_ORB_R     18                        /* 球半径（MID+ 档） */
#define CHAT_ORB_RIP   7                         /* 涟漪环步距（phase 0..2） */
#define CHAT_ORB_WIN   (CHAT_ORB_R + 2 * CHAT_ORB_RIP + 5) /* 动画窗半边=37 */
#define CHAT_ORB_OK    (layout_profile_get()->kind >= LAYOUT_MID) /* 档位门槛 */

static int ui_chat_orb_cx(void) { return epd_gfx_width() / 2; }
static int ui_chat_orb_cy(void)               /* 内容区 36% 线（球窗下方 */
{                                             /* 留状态词+辅助行两行） */
    return UI_STATUS_H + (epd_gfx_height() - UI_STATUS_H) * 9 / 25;
}

/* 画球到帧缓冲（不 flush）：phase -1 静态球；0..2 涟漪帧（环
 * r=R+4+phase*RIP，3px 线宽）。窗口整擦保证环移动无残帧 */
static void ui_chat_orb_draw(int phase)
{
    int cx = ui_chat_orb_cx(), cy = ui_chat_orb_cy();
    epd_gfx_fill_rect(cx - CHAT_ORB_WIN, cy - CHAT_ORB_WIN,
                      2 * CHAT_ORB_WIN, 2 * CHAT_ORB_WIN, EPD_GFX_WHITE);
    ui_chat_fill_circle(cx, cy, CHAT_ORB_R, EPD_GFX_BLACK);
    if (phase >= 0) {
        int rr = CHAT_ORB_R + 4 + phase * CHAT_ORB_RIP;
        ui_chat_fill_circle(cx, cy, rr + 2, EPD_GFX_BLACK);
        ui_chat_fill_circle(cx, cy, rr - 1, EPD_GFX_WHITE);
    }
}

/* 居中状态词（cjk 测宽居中；越界钳到边距） */
static void ui_chat_caption(int y, const char *s, int level)
{
    int x = (epd_gfx_width() - cjk_text_width(level, s)) / 2;
    if (x < UI_MARGIN_X) x = UI_MARGIN_X;
    cjk_text_draw(x, y, level, s, EPD_GFX_BLACK);
}

void ui_render_chat(chat_state_t st, const char *text)
{
    if (!epd_gfx_partial_supported()) return;   /* 三色降级：纯语音+震动 */
    if (page_router_top_owns_display())
        return;                              /* 顶层覆盖层/LAN 期间不绘制 */

    epd_gfx_fill_rect(0, UI_STATUS_H, epd_gfx_width(),
                      epd_gfx_height() - UI_STATUS_H, EPD_GFX_WHITE);

    switch (st) {
    case CHAT_STATE_IDLE: {
        /* 最优方案（2026-09-01）：title 删除（菜单已选，二次确认冗余）；
         * MID+ 小球锚点（与过程态视觉语言连贯）+ 完整回复回看 3 行
         * （静态驻留红利，中段句可回看）；TINY/SMALL 保持纯文字版式 */
        if (CHAT_ORB_OK) {
            int ocx = ui_chat_orb_cx();
            int ocy = UI_STATUS_H +
                      (epd_gfx_height() - UI_STATUS_H) * 6 / 25;
            epd_gfx_fill_rect(ocx - 16, ocy - 16, 32, 32, EPD_GFX_WHITE);
            ui_chat_fill_circle(ocx, ocy, 12, EPD_GFX_BLACK);
            ui_chat_caption(ocy + 22, "按中键说话 · 长按退出", 2);
            const char *fr = chat_mode_full_reply();
            if (fr[0])
                cjk_text_draw_wrap_page(UI_MARGIN_X, ocy + 54,
                                        UI_BODY_MAX_W, UI_MEAN_LEVEL,
                                        UI_BODY_LH, 3, 0, fr, EPD_GFX_BLACK);
            if (chat_mode_wordhit_count() > 0) {
                char hbuf[40];
                snprintf(hbuf, sizeof(hbuf), "生词 %d · SET 收藏",
                         chat_mode_wordhit_count());
                cjk_text_draw(UI_MARGIN_X, ocy + 54 + 3 * UI_BODY_LH + 4,
                              UI_MEAN_LEVEL, hbuf, EPD_GFX_BLACK);
            }
        } else {
            cjk_text_draw(UI_MARGIN_X, UI_WORD_BASE, UI_MEAN_LEVEL,
                          "按中键说话 · 长按中键退出", EPD_GFX_BLACK);
            if (chat_mode_wordhit_count() > 0) {
                char hbuf[40];
                snprintf(hbuf, sizeof(hbuf), "生词 %d · SET 收藏",
                         chat_mode_wordhit_count());
                cjk_text_draw(UI_MARGIN_X, UI_BODY_TOP, UI_MEAN_LEVEL,
                              hbuf, EPD_GFX_BLACK);
            }
            const char *fr = chat_mode_full_reply();
            if (fr[0])
                cjk_text_draw_wrap_page(UI_MARGIN_X, UI_BODY_TOP + UI_BODY_LH,
                                        UI_BODY_MAX_W, UI_MEAN_LEVEL,
                                        UI_BODY_LH, 2, 0, fr, EPD_GFX_BLACK);
        }
        break;
    }
    case CHAT_STATE_RECORDING:
        if (CHAT_ORB_OK) {
            ui_chat_orb_draw(-1);
            ui_chat_caption(ui_chat_orb_cy() + CHAT_ORB_WIN + 6,
                            "聆听中 · · ·", 2);
            ui_chat_caption(ui_chat_orb_cy() + CHAT_ORB_WIN + 34,
                            "请说话 · 停顿即发送", 1);
        } else {
            epd_gfx_draw_text(UI_MARGIN_X, UI_WORD_BASE, "Listening...",
                              EPD_GFX_BLACK, 2);
            cjk_text_draw(UI_MARGIN_X, UI_BODY_TOP, UI_MEAN_LEVEL,
                          "请说话 · 停顿即发送 / 中键立即发", EPD_GFX_BLACK);
        }
        break;
    case CHAT_STATE_UPLOADING:
        if (CHAT_ORB_OK) {
            ui_chat_orb_draw(-1);
            /* 录音时长即时反馈（本地可算零网络）：说话中页面静默的
             * 首个补偿信号——至少确认采到了多长的音 */
            char ucap[40];
            int rms_ms = chat_mode_rec_ms();
            if (rms_ms > 0)
                snprintf(ucap, sizeof(ucap), "已录 %d.%d 秒 · 发送中",
                         rms_ms / 1000, (rms_ms % 1000) / 100);
            else
                snprintf(ucap, sizeof(ucap), "发送中 · · ·");
            ui_chat_caption(ui_chat_orb_cy() + CHAT_ORB_WIN + 6, ucap, 2);
        } else {
            epd_gfx_draw_text(UI_MARGIN_X, UI_WORD_BASE, "Sending...",
                              EPD_GFX_BLACK, 2);
        }
        break;
    case CHAT_STATE_THINKING:
        if (CHAT_ORB_OK) {
            ui_chat_orb_draw(0);          /* 静态首帧；涟漪由 anim_tick 推进 */
            ui_chat_caption(ui_chat_orb_cy() + CHAT_ORB_WIN + 6,
                            "思考中 · · ·", 2);
            /* 识别文本回显（meta.transcript 到达时 set_state 同态重入）：
             * 「你说：…」= mic 正常的最强证据；空串=后端未听到 */
            const char *heard = chat_mode_heard();
            if (heard) {
                char hbuf[160];
                if (heard[0])
                    snprintf(hbuf, sizeof(hbuf), "你说：%s", heard);
                else
                    snprintf(hbuf, sizeof(hbuf), "未听到内容");
                cjk_text_draw_wrap_page(UI_MARGIN_X,
                                        ui_chat_orb_cy() + CHAT_ORB_WIN + 32,
                                        UI_BODY_MAX_W, UI_MEAN_LEVEL,
                                        UI_BODY_LH, 2, 0, hbuf, EPD_GFX_BLACK);
            }
        } else {
            epd_gfx_draw_text(UI_MARGIN_X, UI_WORD_BASE, "Thinking...",
                              EPD_GFX_BLACK, 2);
            const char *heard = chat_mode_heard();
            if (heard && heard[0])
                cjk_text_draw_wrap_page(UI_MARGIN_X, UI_BODY_TOP,
                                        UI_BODY_MAX_W, UI_MEAN_LEVEL,
                                        UI_BODY_LH, 1, 0, heard,
                                        EPD_GFX_BLACK);
        }
        break;
    case CHAT_STATE_PLAYING: {
        /* 句进度（中途打断决策依据）：TTS 降级文本先行未开播（N=0）
         * 只显状态词；TINY/SMALL 保持 FreeSans 原样 */
        if (CHAT_ORB_OK) {
            char cap[40];   /* 「正在回答 · 第 N 句」最坏 33B（%d 11 位）+余量 */
            int no = chat_mode_sentence_no();
            if (no > 0)
                snprintf(cap, sizeof(cap), "正在回答 · 第 %d 句", no);
            else
                snprintf(cap, sizeof(cap), "正在回答");
            ui_chat_caption(UI_WORD_BASE, cap, 2);
        } else {
            epd_gfx_draw_text(UI_MARGIN_X, UI_WORD_BASE, "Speaking",
                              EPD_GFX_BLACK, 2);
        }
        cjk_text_draw(UI_MARGIN_X, UI_BODY_TOP, UI_MEAN_LEVEL,
                      "中键打断重说", EPD_GFX_BLACK);
        break;
    }
    case CHAT_STATE_NETFAIL:
        epd_gfx_draw_text(UI_MARGIN_X, UI_WORD_BASE, "Offline",
                          EPD_GFX_BLACK, 2);
        cjk_text_draw(UI_MARGIN_X, UI_BODY_TOP, UI_MEAN_LEVEL,
                      "网络不可用 · 按中键重试", EPD_GFX_BLACK);
        break;
    }

    /* 当前句 ≤2 行（仅 PLAYING：IDLE 回看已自分档自画，过程态不画 */
    if (text && text[0] && st == CHAT_STATE_PLAYING)
        cjk_text_draw_wrap_page(UI_MARGIN_X, UI_BODY_TOP + 2 * UI_BODY_LH,
                                UI_BODY_MAX_W, UI_MEAN_LEVEL, UI_BODY_LH, 2,
                                0, text, EPD_GFX_BLACK);

    epd_gfx_flush_window(0, UI_STATUS_H, epd_gfx_width(),
                         epd_gfx_height() - UI_STATUS_H);
    LOG_I("chat ui state=%d", (int)st);
}

/* THINKING 涟漪帧：chat_task 读流循环周期调用（500ms 读超时回环点），
 * 内部 600ms 节拍防抖（n>0 连续到达时不加速）；非 THINKING 态/
 * 顶层覆盖层期间 no-op。仅重刷球窗（74×74，A2 快刷 ~100ms） */
void ui_chat_anim_tick(void)
{
    static int64_t last_us = -1;
    static int phase = 0;
    if (!epd_gfx_partial_supported() || !CHAT_ORB_OK) return;
    if (page_router_top_owns_display())
        return;
    if (chat_mode_state() != CHAT_STATE_THINKING) return;
    int64_t now = esp_timer_get_time();
    if (last_us > 0 && now - last_us < 600000) return;
    last_us = now;
    ui_chat_orb_draw(phase);
    int cx = ui_chat_orb_cx(), cy = ui_chat_orb_cy();
    epd_gfx_flush_window(cx - CHAT_ORB_WIN, cy - CHAT_ORB_WIN,
                         2 * CHAT_ORB_WIN, 2 * CHAT_ORB_WIN);
    phase = (phase + 1) % 3;
}
