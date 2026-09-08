/**
 * @file quiz_ui.c
 * @brief 快速测验视图（v1.2 T2.2 建块 / T1.2 自 main.cpp 迁出，修 A1）
 *
 * 迁移口径（T1.2，2026）：函数体零改动——仅导出名变更
 * （quiz_flow_start→quiz_ui_start / ui_draw_quiz→quiz_ui_render /
 * quiz_on_button→quiz_ui_on_button）与三处跨文件接线：
 *   1. ui_fit_font/ui_word_start_size 经 quiz_ui_set_font_fit 注入
 *      （main.cpp static 工具，避免反向依赖）；
 *   2. s_last_mode=MODE_COUNT 改调 main.cpp 导出的
 *      ui_force_full_refresh_next()（同一实现）；
 *   3. ui_render_word 沿 menu_ui 先例 extern 调用（ui_render_current
 *      兼容别名随 T2.2 栈化退役，退出路径改 quiz_page_leave）。
 *
 * 布局宏自 main.cpp 同步复制（值与派生式逐字节一致，视觉零变化；
 * T1.5 参数表落地时与 main.cpp 一并收敛进 layout_profile）。
 */
#include "quiz_ui.h"

#include <stdio.h>
#include <string.h>

#include "debug_log.h"
#include "gpio_config.h"          /* AUDIO_DIR */
#include "epd_driver.h"           /* epd_gfx_* */
#include "audio_player.h"         /* audio_play_file */
#include "haptic.h"               /* haptic_event/HAPTIC_* */
#include "quiz_session.h"         /* 出题核心（纯 C，泛化回调） */
#include "storage_manager.h"      /* storage_file_exists */
#include "word_parser.h"          /* WordEntry/word_parser_* */
#include "cjk_text.h"             /* cjk_text_* 点阵混排 */
#include "layout_profile.h"       /* layout_profile_get/LAYOUT_* */
#include "learning_state.h"       /* learning_state_* */
#include "settings_ui.h"          /* settings_audio_enabled/settings_quiz_grid */
#include "study_mode_machine.h"   /* study_mode_exit_quiz/MODE_QUIZ */
#include "page_router.h"          /* T2.2 栈化：pop_if/render_top */

#include <esp_random.h>           /* esp_fill_random（esp32 直调） */
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>        /* vTaskDelay（反馈停留） */

static const char *TAG = "QUIZ_UI";   /* debug_log.h LOG 宏依赖 */

/* main.cpp 导出的渲染入口（menu_ui.c 同款 extern 先例） */
extern void ui_render_word(study_mode_t mode, int index);
extern void ui_force_full_refresh_next(void);

/* ---- 布局宏（档位参数经 layout_profile 字段引用，T1.5 收敛完成；
 * 字号/行距派生式仍为本文件局部）---- */
#define UI_TINY         (layout_profile_get()->kind == LAYOUT_TINY)
#define UI_STATUS_H     (layout_profile_get()->status_h)  /* 状态栏高度（T1.5） */
#define UI_MARGIN_X     (layout_profile_get()->margin_x)   /* 左右留白（T1.5） */
#define UI_STATUS_BASE  (UI_STATUS_H - 10)   /* 状态栏文字基线 */
#define UI_MEAN_LEVEL   (layout_profile_get()->kind <= LAYOUT_SMALL \
                         ? (settings_font_mode() >= 1 ? 1 \
                            : (layout_profile_get()->narrow_tiny ? 1 : 0)) \
                         : (layout_profile_get()->mean_level >= 3 ? 3 \
                            : layout_profile_get()->mean_level \
                              + (settings_font_mode() >= 1 ? 1 : 0)))  /* 题干
 * 正文级 2026-09-08：MID+ 默认读 mean_level（PPI 自动层 3.4mm 目标） */
#define UI_BODY_MAX_W   (epd_gfx_width() - 2 * UI_MARGIN_X)
#define UI_BODY_LH      (UI_TINY ? (UI_MEAN_LEVEL ? 26 : 20) \
                         : (UI_MEAN_LEVEL >= 3 ? 36 : (UI_MEAN_LEVEL ? 24 : 20)))
#define UI_FOOT_TOP     (epd_gfx_height() - 16)        /* 底部提示行基线 */
#define RV_ITEM_H   (layout_profile_get()->item_h)  /* 对齐 menu_ui 列表行高（T1.5） */
#define RV_HINT_H   (layout_profile_get()->rv_hint_h)  /* 底部提示行预留（T1.5） */
/* 测验纵列选项区顶（题干 1/3 内容区）；行高在 ui_draw_quiz_option
 * 内由可用区四等分与 RV_ITEM_H 取小（2026-08-25 真机反馈收窄：
 * QUIZ_DESIGN §5「一屏四行」在 MID 416x240 沿用 RV_ITEM_H=44 实测
 * 违约——opt_top 93 + 4×44 = 269 出屏（提示栏 206），D 项不可见且
 * 测验选项恒 4 项无滚动语义；收窄后 MID 416x240 行高 27、400x300=37，
 * TINY 竖屏充裕仍 28，SMALL 恒网格不进纵列） */
#define QZ_OPT_TOP  (UI_STATUS_H + (epd_gfx_height() - UI_STATUS_H \
                                    - RV_HINT_H) / 3)

/* ---- 快速测验视图（v1.2 T2.2，QUIZ_DESIGN P1-b）：题池重映射 + 渲染 +
 * 作答编排（T1.2 自 main.cpp 迁出）；出题核心 quiz_session 纯 C
 * （T2.1，native-test 验证）。核心域词条索引 [0,n) 经 s_quiz_pool
 * 重映射到真词索引——泛化约束：核心不碰 learning_state / word_parser ---- */
#define QUIZ_ROUND_N    10    /* 每轮题数（QUIZ_DESIGN §3） */
#define QUIZ_POOL_MAX   64    /* 题池上限：到期 + 新词补足 + 随机兜底 */
#define QUIZ_FB_OK_MS   400   /* 答对反馈停留（高亮保留一拍） */
#define QUIZ_FB_ERR_MS  350   /* 答错反馈每拍（两拍约 0.7s，§5） */
static int    s_quiz_pool[QUIZ_POOL_MAX];  /* 核心域 idx → 真词索引 */
static int    s_quiz_pool_n = 0;
static int    s_quiz_total  = 0;   /* 本轮实际题数（start 返回） */
static int    s_quiz_i = 0;        /* 当前题号 */
static int    s_quiz_sel = 0;      /* 选项光标 0=A..3=D */
static int    s_quiz_correct = 0, s_quiz_wrong = 0;
static bool   s_quiz_summary = false;   /* 小结页态（任意键退出） */
static int8_t s_quiz_fb = -1;      /* 反馈：-1 无 / 1 对 / 0 错拍1 / 2 错拍2 */
static int8_t s_quiz_fb_pick = -1; /* 答错时用户选择槽（打 ×） */

/* RNG 注入：esp_fill_random 语义包装（硬件 RNG，逐次抽取） */
static uint32_t quiz_rnd(uint32_t bound)
{
    uint32_t v = 0;
    if (!bound) return 0;
    esp_fill_random(&v, sizeof(v));
    return v % bound;
}

/* 文本回调（泛化约束适配层，QUIZ_DESIGN §7）：slot 0=题干单词 text /
 * slot 1=选项释义首行（多行取首行，复习词表右列同源）/ slot 2=前缀
 * （v1.5 T5.1 同首字母干扰：text 首个 UTF-8 码点，静态单缓冲——核心
 * 侧先拷贝题干前缀再查候选，见 quiz_session.c 阶段 A）；slot 0/1 双
 * 缓冲轮转供核心 same_opt_text 的两指针 strcmp 同时有效 */
static char s_quiz_line[2][WORD_MEANING_MAX];
static int  s_quiz_line_i = 0;
static char s_quiz_prefix[8];
static const char *quiz_txt(int word_idx, int slot)
{
    const WordEntry *w = word_parser_get(s_quiz_pool[word_idx]);
    if (!w || !w->meaning[0]) return "";

    if (slot == 2) {          /* 前缀：首码点长度判定（ASCII 1B / CJK ≤4B） */
        if (!w->text[0]) return "";
        size_t n = 1;
        unsigned char c = (unsigned char)w->text[0];
        if (c >= 0xF0) n = 4;
        else if (c >= 0xE0) n = 3;
        else if (c >= 0xC0) n = 2;
        if (n >= sizeof(s_quiz_prefix)) n = sizeof(s_quiz_prefix) - 1;
        memcpy(s_quiz_prefix, w->text, n);
        s_quiz_prefix[n] = '\0';
        return s_quiz_prefix;
    }
    if (slot == 0) return w->text;

    char *buf = s_quiz_line[s_quiz_line_i = !s_quiz_line_i];
    const char *nl = strchr(w->meaning, '\n');
    size_t len = nl ? (size_t)(nl - w->meaning) : strlen(w->meaning);
    if (len >= sizeof(s_quiz_line[0])) len = sizeof(s_quiz_line[0]) - 1;
    memcpy(buf, w->meaning, len);
    buf[len] = '\0';
    return buf;
}

/* 词音路径解析（speak 动作同源规则，study_mode_machine 不改）：audio
 * 人工命名词库优先，否则云端约定 {cloud_id}.mp3；无源返回 false */
static bool word_audio_path(const WordEntry *w, char *buf, size_t n)
{
    if (!w) return false;
    if (w->audio[0])
        snprintf(buf, n, "%s/%s", AUDIO_DIR, w->audio);
    else if (w->cloud_id[0])
        snprintf(buf, n, "%s/%s.mp3", AUDIO_DIR, w->cloud_id);
    else
        return false;
    return true;
}

/* T3 可用性探测（v1.5 T5.1，QUIZ_DESIGN 开放问题 1 定案：SD 缺音频
 * 逐题降级 T1，会话开启时一次探完）：发音设置关=整体禁用 T3 */
static int quiz_audio_ok(int word_idx)
{
    if (!settings_audio_enabled()) return 0;
    const WordEntry *w = word_parser_get(s_quiz_pool[word_idx]);
    char path[128];
    return (w && word_audio_path(w, path, sizeof(path)) &&
            storage_file_exists(path)) ? 1 : 0;
}

/* T3 自动播题音（首题/换题后）：异步入队即返；存在性已在会话开启时
 * 探过，中途删文件的入队失败由音频层自理（不阻断作答） */
static void quiz_autoplay(const quiz_question_t *q)
{
    if (q->type != QUIZ_T3) return;
    const WordEntry *w = word_parser_get(s_quiz_pool[q->word_idx]);
    char path[128];
    if (w && word_audio_path(w, path, sizeof(path)) &&
        audio_play_file(path) != 0)
        LOG_W("quiz autoplay rejected");
}

/* 中键重播（2×2 直选模式：T3 必备 / T1·T2 听音复核）：缺源短震
 * （speak 动作同款反馈，不回退测试音） */
static void quiz_replay(void)
{
    quiz_question_t q;
    if (!quiz_session_at(s_quiz_i, &q)) return;
    const WordEntry *w = word_parser_get(s_quiz_pool[q.word_idx]);
    char path[128];
    if (!w || !word_audio_path(w, path, sizeof(path)) ||
        !storage_file_exists(path)) {
        haptic_event(HAPTIC_ERROR);
        return;
    }
    if (audio_play_file(path) != 0)
        LOG_W("quiz replay rejected");
}

/* 2×2 方向直选判定（T5.1）：T3 恒直选（四向四选项天然配套，中键留给
 * 重播——纵列 T3 重播改长按中，仅 TINY 档）；T1/T2 由「测验快答」设置
 * 控制（默认关=纵列基线，真机对比后定夺，QUIZ_DESIGN §5 P2 变体）；
 * T5.2 判断题恒纵列（两选项不适合 2×2，左右键已直答） */
static bool quiz_grid_active(void)
{
    quiz_question_t q;
    if (!quiz_session_at(s_quiz_i, &q)) return false;
    /* TINY 恒纵列（2026-08-25）：与 SMALL 互为镜像——SMALL 高度枯竭
     * 恒网格，TINY 宽度枯竭恒纵列（格宽 (W-2×8-4)/2 ≈ 51px，格内
     * 释义 16px 仅 2 字/行×2 行不可读；竖屏 4×28 行距余量足）；
     * T3 让出中键作答后重播迁长按中（quiz_ui_on_button） */
    if (UI_TINY) return false;
    if (q.type == QUIZ_T3) return true;
    if (q.type == QUIZ_TF) return false;
    /* SMALL 264x176 纵列四行数学上放不下（可用 70px < 4×16px 释义行），
     * 恒 2×2 网格（P2 变体本为小屏省空间设计，2026-08-25）；
     * 用户开关仅 MID+ 档生效 */
    if (layout_profile_get()->kind == LAYOUT_SMALL) return true;
    return settings_quiz_grid();
}

/* 题池构造（QUIZ_DESIGN §3）：到期词优先（due 视图序）→ 未学新词
 * 补足 → 随机全库兜底（跨阶段去重）；池即核心域 [0, n)，干扰项
 * 从池内排除本轮题干后采样（到期词互为干扰，难度更真实） */
static void quiz_pool_build(void)
{
    int total = word_parser_get_count();
    int n = 0;

    for (int i = 0; n < QUIZ_POOL_MAX; i++) {         /* 1) 到期词优先 */
        int wi = learning_state_due_at(i);
        if (wi < 0) break;
        s_quiz_pool[n++] = wi;
    }
    for (int i = 0; i < total && n < QUIZ_POOL_MAX; i++) {  /* 2) 新词补足 */
        /* 墨封词排除（2026-09-04 过滤矩阵：已声明熟练不再考；
         * 干扰项与随机兜底不过滤） */
        if (learning_state_is_new(i) && !learning_state_is_mastered(i))
            s_quiz_pool[n++] = i;
    }
    for (int tries = 0; n < QUIZ_POOL_MAX && tries < QUIZ_POOL_MAX * 8;
         tries++) {                                    /* 3) 随机兜底去重 */
        int wi = (int)quiz_rnd((uint32_t)total);
        bool dup = wi < 0;
        for (int k = 0; k < n && !dup; k++)
            if (s_quiz_pool[k] == wi) dup = true;
        if (!dup) s_quiz_pool[n++] = wi;
    }
    s_quiz_pool_n = n;
}

/* 单词字号适配注入存储 + 包装（T1.2）：main.cpp 的 static 工具经
 * quiz_ui_set_font_fit 注入（setup 必经，NULL 仅防御） */
static int (*s_fit_font)(const char *, int, int) = NULL;
static int (*s_word_start_size)(void) = NULL;

void quiz_ui_set_font_fit(int (*fit_font)(const char *text, int start_size,
                                          int max_w),
                          int (*word_start_size)(void))
{
    s_fit_font = fit_font;
    s_word_start_size = word_start_size;
}

static int quiz_fit_font(const char *text, int max_w)
{
    if (!s_fit_font || !s_word_start_size) return 2;   /* 防御：P1b 前口径 */
    return s_fit_font(text, s_word_start_size(), max_w);
}

/* ---- T2.2 页面路由接入（browse_mode 栈化同款先例） ---- */
/* 栈顶 render：与 ui_render_word 的 MODE_QUIZ 分流同路径（首帧与
 * 后续重绘共用刷新编排，模式切换自然全刷） */
static void quiz_page_render(void)
{
    ui_render_word(MODE_QUIZ, 0);
}

/* 栈顶按键：全转发（作答/跳过/退出编排见 quiz_ui_on_button） */
static bool quiz_page_on_button(nav_key_t id, button_event_t event)
{
    quiz_ui_on_button(id, event);
    return true;
}

/* 出栈编排：pop_if 归位后 render_top 回 base 分流；本文件 6 处
 * 退出点（防御/小结/RST/默认分支）统一改走此入口 */
static void quiz_page_leave(void)
{
    page_router_exit(&g_quiz_page);   /* P2 路由补完：退出两连收敛 */
}

const page_t g_quiz_page = { "quiz", quiz_page_render, quiz_page_on_button,
                             quiz_ui_start, NULL, false };

/* 进入测验会话（push 的 enter 回调，自绘首帧）：
 * 题池 → 核心 start → 首帧（模式切换自然全刷） */
void quiz_ui_start(void)
{
    quiz_pool_build();
    s_quiz_i = 0;
    s_quiz_sel = 0;
    s_quiz_correct = 0;
    s_quiz_wrong = 0;
    s_quiz_summary = false;
    s_quiz_fb = -1;
    /* v1.5 T5.1：T3 探测回调（SD 缺音频逐题降级 T1）+ 同首字母
     * 干扰偏好（slot 2 前缀语义，全科目泛化：英文首字母 / CJK 首字）；
     * T5.2：判断题（i%4==3 槽位，陈述真假构造在核心） */
    quiz_cfg_t cfg;
    cfg.txt = quiz_txt;
    cfg.rnd = quiz_rnd;
    cfg.audio_ok = quiz_audio_ok;
    cfg.flags = QUIZ_F_SAME_PREFIX | QUIZ_F_TRUE_FALSE;
    s_quiz_total = quiz_session_start_ex(s_quiz_pool_n, QUIZ_ROUND_N, &cfg);
    if (s_quiz_total <= 0) {   /* 池 <8 防御（enter_quiz 前置应已挡） */
        study_mode_exit_quiz();
        quiz_page_leave();
        return;
    }
    ui_render_word(MODE_QUIZ, 0);
    quiz_question_t q0;
    if (quiz_session_at(0, &q0)) quiz_autoplay(&q0);   /* T3 首题自动播 */
}

/* 选项行渲染：槽号 A-D + 释义首行 16px 点阵；反选=黑底白字
 * （复习词表同款）；× = 答错标记（用户所选槽）。行高可用区四等分
 * 与 RV_ITEM_H 取小（见 QZ_OPT_TOP 注释，非复习词表 RV_ITEM_H） */
static void ui_draw_quiz_option(int k, const char *txt, bool invert,
                                bool mark_x, int opt_top)
{
    int qz_h = ((UI_TINY ? epd_gfx_height() : UI_FOOT_TOP - 4) - opt_top)
               / QUIZ_OPTS;
    if (qz_h > RV_ITEM_H) qz_h = RV_ITEM_H;

    int y = opt_top + k * qz_h;
    int w = epd_gfx_width() - 2 * UI_MARGIN_X;
    if (invert)
        epd_gfx_fill_rect(UI_MARGIN_X, y, w, qz_h - 4, EPD_GFX_BLACK);

    char slot[3] = { (char)('A' + k), '.', 0 };
    epd_gfx_draw_text(UI_MARGIN_X + 4, y + qz_h * 3 / 4,
                      slot, invert ? EPD_GFX_WHITE : EPD_GFX_BLACK, 2);
    int tw, th;
    epd_gfx_text_bounds(slot, 2, &tw, &th);
    int mx = UI_MARGIN_X + 4 + tw + 8;
    int mw = UI_MARGIN_X + w - 8 - mx;
    if (mw >= 32 && txt[0])
        cjk_text_draw_wrap(mx, y + (qz_h - 16) / 2, mw, 0, 0, 1,
                           txt, invert ? EPD_GFX_WHITE : EPD_GFX_BLACK);
    if (mark_x)
        epd_gfx_draw_text(UI_MARGIN_X + w - 20, y + qz_h * 3 / 4,
                          "x", EPD_GFX_BLACK, 2);
}

/* 2×2 网格选项格（v1.5 T5.1 快答，QUIZ_DESIGN §5 P2 变体）：槽位映射
 * 左上0/右上1/左下2/右下3（与方向键一一对应）；反选=黑底白字整格，
 * 格间留缝分区；× = 答错标记（用户所选格）；文本垂直居中 ≤2 行 */
static void ui_draw_quiz_cell(int k, const char *txt, bool invert,
                              bool mark_x, int gx, int gy, int cw, int ch)
{
    int x = gx + (k & 1) * (cw + 4);
    int y = gy + (k >> 1) * (ch + 4);
    if (invert)
        epd_gfx_fill_rect(x, y, cw, ch, EPD_GFX_BLACK);
    int fg = invert ? EPD_GFX_WHITE : EPD_GFX_BLACK;

    char slot[3] = { (char)('A' + k), '.', 0 };
    epd_gfx_draw_text(x + 6, y + ch - 5, slot, fg, 1);

    int mw = cw - 16;
    int max_lines = ch >= 40 ? 2 : 1;
    if (mw >= 24 && txt[0]) {
        int lines = cjk_text_wrap_lines(mw, 0, txt);
        if (lines > max_lines) lines = max_lines;
        int ty = y + (ch - lines * 18) / 2 + 1;
        cjk_text_draw_wrap(x + 8, ty, mw, 0, 18, max_lines, txt, fg);
    }
    if (mark_x)
        epd_gfx_draw_text(x + cw - 14, y + ch - 5, "x", EPD_GFX_BLACK, 1);
}

/* 测验视图渲染（QUIZ_DESIGN §5 版式）：自绘状态栏（题号 i+1/N）+
 * 题干三态（v1.5 T5.1：T1 单词+音标 / T2 释义首行 ≤2 行 / T3 听音
 * 提示——不画单词与音标防泄题）+ 选项区双版式（纵列 4 行 / 2×2
 * 网格，quiz_grid_active 分派；反馈态同语义：对=选中高亮保留，
 * 错拍1=正确项反白+错选项 ×、拍2=反白恢复 × 保留）；小结页
 * 「对 n · 错 m」居中 + 任意键退出 */
void quiz_ui_render(void)
{
    /* 状态栏：模式名「测验」+ 题号（小结页显示 N/N）+ 分隔线 */
    epd_gfx_fill_rect(0, 0, epd_gfx_width(), UI_STATUS_H, EPD_GFX_WHITE);
    cjk_text_draw(UI_MARGIN_X, (UI_STATUS_H - 16) / 2, 0,
                  "测验", EPD_GFX_BLACK);
    char buf[24];
    snprintf(buf, sizeof(buf), "%d/%d",
             s_quiz_summary ? s_quiz_total : s_quiz_i + 1, s_quiz_total);
    int tw, th;
    epd_gfx_text_bounds(buf, 1, &tw, &th);
    epd_gfx_draw_text(epd_gfx_width() - UI_MARGIN_X - tw, UI_STATUS_BASE,
                      buf, EPD_GFX_BLACK, 1);
    epd_gfx_draw_hline(UI_MARGIN_X, UI_STATUS_H,
                       epd_gfx_width() - 2 * UI_MARGIN_X, EPD_GFX_BLACK);

    epd_gfx_fill_rect(0, UI_STATUS_H, epd_gfx_width(),
                      epd_gfx_height() - UI_STATUS_H, EPD_GFX_WHITE);

    if (s_quiz_summary) {          /* 小结页：居中统计 + 退出提示 */
        snprintf(buf, sizeof(buf), "对 %d · 错 %d",
                 s_quiz_correct, s_quiz_wrong);
        int w1 = cjk_text_width(UI_MEAN_LEVEL, buf);
        cjk_text_draw((epd_gfx_width() - w1) / 2,
                      (epd_gfx_height() - UI_STATUS_H) / 2 + UI_STATUS_H - 16,
                      UI_MEAN_LEVEL, buf, EPD_GFX_BLACK);
        const char *h = "任意键退出";
        int w2 = cjk_text_width(0, h);
        cjk_text_draw((epd_gfx_width() - w2) / 2,
                      (epd_gfx_height() - UI_STATUS_H) / 2 + UI_STATUS_H + 12,
                      0, h, EPD_GFX_BLACK);
        return;
    }

    quiz_question_t q;
    if (!quiz_session_at(s_quiz_i, &q)) return;

    bool grid = quiz_grid_active();
    const WordEntry *w = word_parser_get(s_quiz_pool[q.word_idx]);
    int opt_top = QZ_OPT_TOP;
    if (grid)                       /* 网格版式：题干 1/3 + 网格 2/3 */
        opt_top = UI_STATUS_H + (UI_FOOT_TOP - UI_STATUS_H) / 3;

    /* 题干四态（T5.1/T5.2）：T3 不画单词/音标（听觉题泄题防线） */
    if (q.type == QUIZ_T3) {
        int sh = opt_top - UI_STATUS_H;
        const char *t1 = "听音辨词";
        int w1 = cjk_text_width(1, t1);
        cjk_text_draw((epd_gfx_width() - w1) / 2, UI_STATUS_H + sh / 2 - 22,
                      1, t1, EPD_GFX_BLACK);
        /* 2026-08-25：TINY 纵列版提示改「长按中 重播」（5+2 字 88px
         * 可容纳；grid 版全宽文案 176px 超 106/112px 正文宽） */
        const char *t2 = grid ? "中键重播 · 方向选义" : "长按中 重播";
        int w2 = cjk_text_width(0, t2);
        cjk_text_draw((epd_gfx_width() - w2) / 2, UI_STATUS_H + sh / 2 + 6,
                      0, t2, EPD_GFX_BLACK);
    } else if (q.type == QUIZ_T2) {
        /* 释义题干（QUIZ_DESIGN §2 可 2 行）：首行文本左对齐顶部起排 */
        cjk_text_draw_wrap(UI_MARGIN_X, UI_STATUS_H + 6, UI_BODY_MAX_W,
                           UI_MEAN_LEVEL, UI_BODY_LH, 2,
                           quiz_txt(q.word_idx, 1), EPD_GFX_BLACK);
    } else if (q.type == QUIZ_TF) {
        /* 判断题干（T5.2）：单词大字（题干区上部 2/3）+ 待判释义行
         * （opt_word[1] 释义来源：真=题词 / 假=干扰词），音标略去
         * （视觉降噪，判断焦点在词义配对本身）；释义行紧贴选项区顶，
         * 词基线上移 20px 让位防重叠（TINY 档 sh 仅 26 亦成立） */
        if (w)
            epd_gfx_draw_text(UI_MARGIN_X,
                              UI_STATUS_H + (opt_top - UI_STATUS_H - 20) * 2 / 3,
                              w->text, EPD_GFX_BLACK,
                              quiz_fit_font(w->text, UI_BODY_MAX_W));
        cjk_text_draw_wrap(UI_MARGIN_X, opt_top - 20, UI_BODY_MAX_W,
                           0, 18, 1, quiz_txt(q.opt_word[1], 1),
                           EPD_GFX_BLACK);
    } else if (w) {
        /* T1：单词大字（全宽自适应降字号）+ 音标（斜杠包裹惯例） */
        int stem_base = UI_STATUS_H + (opt_top - UI_STATUS_H) * 2 / 3;
        epd_gfx_draw_text(UI_MARGIN_X, stem_base, w->text, EPD_GFX_BLACK,
                          quiz_fit_font(w->text, UI_BODY_MAX_W));
        if (w->phonetic[0] && opt_top - stem_base - 6 >= 18) {
            if (w->phonetic[0] == '/' || w->phonetic[0] == '[')
                cjk_text_draw(UI_MARGIN_X, stem_base + 6, 0,
                              w->phonetic, EPD_GFX_BLACK);
            else {
                char ph[WORD_PHONETIC_MAX + 4];
                snprintf(ph, sizeof(ph), "/%s/", w->phonetic);
                cjk_text_draw(UI_MARGIN_X, stem_base + 6, 0,
                              ph, EPD_GFX_BLACK);
            }
        }
    }

    /* 选项区：反选高亮；反馈态覆盖（见函数头） */
    if (grid) {
        int bot = UI_TINY ? (epd_gfx_height() - 4) : (UI_FOOT_TOP - 4);
        int cw = (epd_gfx_width() - 2 * UI_MARGIN_X - 4) / 2;
        int ch = (bot - opt_top - 4) / 2;
        if (cw < 24) cw = 24;
        if (ch < 20) ch = 20;
        for (int k = 0; k < QUIZ_OPTS; k++) {
            bool invert = false, mark_x = false;
            if (s_quiz_fb < 0 || s_quiz_fb == 1)
                invert = (k == s_quiz_sel);
            else if (s_quiz_fb == 0) {
                invert = (k == q.answer);
                mark_x = (k == s_quiz_fb_pick);
            } else {
                mark_x = (k == s_quiz_fb_pick);
            }
            ui_draw_quiz_cell(k, quiz_txt(q.opt_word[k], 1),
                              invert, mark_x, UI_MARGIN_X, opt_top, cw, ch);
        }
    } else {
        /* 纵列版式：判断题两行固定文案（T5.2），四选一四行词条释义 */
        static const char *tf_lbl[2] = { "错", "对" };
        int opts_n = (q.type == QUIZ_TF) ? 2 : QUIZ_OPTS;
        for (int k = 0; k < opts_n; k++) {
            bool invert = false, mark_x = false;
            if (s_quiz_fb < 0 || s_quiz_fb == 1)
                invert = (k == s_quiz_sel);       /* 常态/答对：选中高亮 */
            else if (s_quiz_fb == 0) {
                invert = (k == q.answer);         /* 错拍1：正确项反白 */
                mark_x = (k == s_quiz_fb_pick);
            } else {
                mark_x = (k == s_quiz_fb_pick);   /* 错拍2：反白恢复 × 保留 */
            }
            ui_draw_quiz_option(k,
                                q.type == QUIZ_TF ? tf_lbl[k]
                                : quiz_txt(q.opt_word[k], 1),
                                invert, mark_x, opt_top);
        }
    }

    if (!UI_TINY) {                /* 提示栏（TINY 档省略，§5） */
        if (q.type == QUIZ_TF)
            cjk_text_draw(UI_MARGIN_X, UI_FOOT_TOP, 0,
                          "上/下 选择 · 左错右对 · 中 重播 · SET 跳过",
                          EPD_GFX_BLACK);
        else if (grid)
            cjk_text_draw(UI_MARGIN_X, UI_FOOT_TOP, 0,
                          "方向 直选 · 中 重播 · SET 跳过 · RST 退出",
                          EPD_GFX_BLACK);
        else
            cjk_text_draw(UI_MARGIN_X, UI_FOOT_TOP, 0,
                          "上/下 选择 · 中 作答 · SET 跳过 · RST 退出",
                          EPD_GFX_BLACK);
    }
}

/* 推进：下一题（局刷新题）或小结页（低频全刷，§5）；T5.1：T3 新题
 * 随帧自动播词音（异步入队，渲染先行不阻塞） */
static void quiz_next(void)
{
    quiz_question_t q;
    s_quiz_fb = -1;
    if (quiz_session_at(s_quiz_i + 1, &q)) {
        s_quiz_i++;
        s_quiz_sel = 0;
        ui_render_word(MODE_QUIZ, 0);
        quiz_autoplay(&q);
    } else {
        s_quiz_summary = true;
        ui_force_full_refresh_next();   /* 强制小结页全刷（低频帧） */
        ui_render_word(MODE_QUIZ, 0);
    }
}

/* 提交作答（中键，QUIZ_DESIGN §4/§5）：对/错映射 quality 4/1 即时入
 * learning_state（与左/右自评同源，今日统计同口径）；反馈为模态短暂
 * 阻塞（对 0.4s / 错两拍 0.7s，反馈期连按自然丢弃）；跳过（SET）
 * 不经本函数——词保留到期状态下轮再推 */
static void quiz_submit(void)
{
    quiz_question_t q;
    if (!quiz_session_at(s_quiz_i, &q)) return;

    int r = quiz_session_answer(s_quiz_i, s_quiz_sel);
    if (r < 0) return;

    learning_state_apply_quality(s_quiz_pool[q.word_idx], r == 1 ? 4 : 1);
    if (r == 1) {
        s_quiz_correct++;
        s_quiz_fb = 1;
        haptic_event(HAPTIC_REVIEW);     /* 对：30ms 短震即切（§5） */
        ui_render_word(MODE_QUIZ, 0);    /* 高亮保留局刷 */
        vTaskDelay(pdMS_TO_TICKS(QUIZ_FB_OK_MS));
    } else {
        s_quiz_wrong++;
        s_quiz_fb = 0;
        s_quiz_fb_pick = (int8_t)s_quiz_sel;
        ui_render_word(MODE_QUIZ, 0);    /* 拍1：正确项反白 + 错项 × */
        vTaskDelay(pdMS_TO_TICKS(QUIZ_FB_ERR_MS));
        s_quiz_fb = 2;
        ui_render_word(MODE_QUIZ, 0);    /* 拍2：反白恢复 × 保留 */
        vTaskDelay(pdMS_TO_TICKS(QUIZ_FB_ERR_MS));
    }
    quiz_next();
}

/* 测验按键路由（同 MODE_CHAT 先例块位；QUIZ_DESIGN §5）：
 * 纵列：上/下=移动选项 A↔D 循环、中=作答；
 * 2×2 直选（T5.1，quiz_grid_active）：上=左上/右=右上/下=左下/
 * 左=右下方向键即答（省中键确认），中=重播（T3 必备/T1·T2 听音
 * 复核）；SET=跳过、RST=中途退出（已答保留评分）；小结页任意键退出 */
void quiz_ui_on_button(nav_key_t id, button_event_t event)
{
    if (s_quiz_summary) {              /* 小结页：任意键退出 */
        study_mode_exit_quiz();
        quiz_page_leave();
        return;
    }

    if (event == BUTTON_EVENT_LONG_PRESS) {
        if (id == NAV_RST) {           /* RST 长/短按均退出（临时视图语义） */
            study_mode_exit_quiz();
            quiz_page_leave();
            return;
        }
        /* TINY 档 T3 纵列重播（2026-08-25）：TINY 恒纵列后中键短按=作答，
         * grid 版「中键即重播」的等价键位迁长按；反馈/小结态不响防止
         * 打断节奏（s_quiz_fb<0 且非小结才生效） */
        if (id == NAV_CENTER && s_quiz_fb < 0) {
            quiz_question_t q3;
            if (quiz_session_at(s_quiz_i, &q3) && q3.type == QUIZ_T3 &&
                !quiz_grid_active())
                quiz_replay();
        }
        return;
    }
    if (event != BUTTON_EVENT_SHORT_PRESS) return;

    {   /* 判断题（T5.2）：恒纵列两选项，左右键直答（两模式均启用），
         * 上/下两槽循环，中键重播（听音复核）；槽 0=错 / 1=对 */
        quiz_question_t qtf;
        if (quiz_session_at(s_quiz_i, &qtf) && qtf.type == QUIZ_TF) {
            switch (id) {
            case NAV_UP:
            case NAV_DOWN:
                s_quiz_sel ^= 1;
                ui_render_word(MODE_QUIZ, 0);
                return;
            case NAV_LEFT:            /* 错 */
                s_quiz_sel = 0;
                quiz_submit();
                return;
            case NAV_RIGHT:           /* 对 */
                s_quiz_sel = 1;
                quiz_submit();
                return;
            case NAV_CENTER:
                quiz_replay();
                return;
            case NAV_SET:
                quiz_next();          /* 跳过：不评分直接换题 */
                return;
            case NAV_RST:
            default:
                study_mode_exit_quiz();
                quiz_page_leave();
                return;
            }
        }
    }

    if (quiz_grid_active()) {          /* 2×2 方向直选（快答模式） */
        switch (id) {
        case NAV_UP:                   /* 左上 */
            s_quiz_sel = 0;
            quiz_submit();
            return;
        case NAV_RIGHT:                /* 右上 */
            s_quiz_sel = 1;
            quiz_submit();
            return;
        case NAV_DOWN:                 /* 左下 */
            s_quiz_sel = 2;
            quiz_submit();
            return;
        case NAV_LEFT:                 /* 右下（左下/右下歧义真机定夺） */
            s_quiz_sel = 3;
            quiz_submit();
            return;
        case NAV_CENTER:
            quiz_replay();
            return;
        case NAV_SET:
            quiz_next();               /* 跳过：不评分直接换题 */
            return;
        case NAV_RST:
        default:
            study_mode_exit_quiz();    /* 中途退出：已答题评分保留 */
            quiz_page_leave();
            return;
        }
    }

    switch (id) {
    case NAV_UP:
        s_quiz_sel = (s_quiz_sel + QUIZ_OPTS - 1) % QUIZ_OPTS;
        ui_render_word(MODE_QUIZ, 0);
        return;
    case NAV_DOWN:
        s_quiz_sel = (s_quiz_sel + 1) % QUIZ_OPTS;
        ui_render_word(MODE_QUIZ, 0);
        return;
    case NAV_CENTER:
        quiz_submit();
        return;
    case NAV_SET:
        quiz_next();                   /* 跳过：不评分直接换题 */
        return;
    case NAV_RST:
        study_mode_exit_quiz();        /* 中途退出：已答题评分保留 */
        quiz_page_leave();
        return;
    default:
        return;
    }
}
