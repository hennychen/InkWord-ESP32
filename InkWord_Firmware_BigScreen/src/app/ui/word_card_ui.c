/**
 * @file word_card_ui.c
 * @brief 大屏词卡页实现（1920x1080）——词典双栏（UI 重设计 2026-09-17）
 *
 * 整体白底黑字（2026-09-17 用户定稿：去顶栏/左栏黑底）+ 词典双栏：
 *   顶栏 0~120：模式名（左）+ 收藏★/墨封徽标 + 序号（右）
 *   主区 120~1000：
 *     左栏 0~640（词典封面）：词头 48pt Bold 自适应降档
 *     （6→5→4 档即 48→36→24pt，宽限 480px；长词自动让位）+ 音标
 *     40px 点阵（字库含 IPA；裸音标自动补 / /）+ 底部徽标行
 *     tag·grade·source
 *     右栏 720~1840（词典正文）：释义 48px 点阵断行
 *     （行距 76，9 行/页），遮蔽时黑块白字提示；多页词内翻页
 *     （mean_page_step），页码右下
 *   底栏 1000~1080：静态键位提示（内容不随词/态变化，永远
 *   不进局刷窗口）
 *
 * 刷新三档（2026-09-17 升级）：首帧/模式切换 = GC16 全刷；翻词/
 *   收藏变化 = DU 全屏窗口（左右栏+顶栏徽标均有变）；同词揭晓/
 *   释义翻页 = DU 右栏窗口 flush_window(720,120,1120,880)——左栏
 *   词头不动是双栏布局的局刷红利。gfx 层 K=8 次局刷自动插全屏
 *   重置驱白灰染（run91 纪律）。
 *
 * 词/遮蔽态变化自动清零释义页码（render 入口检测，编排层零钩子）。
 * 双栏几何为本页专属常量（XLARGE 独占页，同 menu_ui 不读档位）。
 */
#include "word_card_ui.h"

#include <stdio.h>
#include <string.h>

#include "cjk_font.h"
#include "cjk_text.h"
#include "debug_log.h"
#include "epd_gfx.h"
#include "layout_profile.h"
#include "learning_state.h"
#include "study_mode_machine.h"
#include "word_parser.h"

static const char *TAG = "WCARD";

/* ---- 词典双栏几何（1920x1080 专属常量） ---- */
#define TOP_H        120   /* 顶栏高（黑底白字） */
#define BOTTOM_H      80   /* 底栏高（键位提示带） */
#define LEFT_W       640   /* 左栏宽（黑底词头区） */
#define PAD           80   /* 左栏/顶栏左右内边距 */
#define RIGHT_X      720   /* 右栏起点（与左栏间 80px 白底分隔） */
#define RIGHT_W     1120   /* 右栏宽（720~1840，右缘同 PAD 收口） */
#define HEAD_BASE_Y  400   /* 词头基线（48pt Bold；cap 高 ~68px） */
#define PHON_Y       480   /* 音标行顶（40px 级） */
#define BADGE_Y      900   /* 徽标行顶（tag·grade·source，20px） */

/* 释义区排版参数：48px 级 + 行距 76，9 行 = 684px，主区 880 内
 * 垂直居中（首行顶 = 120 + (880-684)/2 = 218） */
#define MEAN_LEVEL     4   /* 释义点阵级（48px） */
#define MEAN_LINE_H   76   /* 释义行距 */
#define MEAN_MAX_LINES 9   /* 每页行数 */
#define MEAN_Y0      218   /* 释义首行顶 */
#define LBL_LEVEL      1   /* 顶栏/音标外标签/提示点阵级（20px） */
#define PHON_LEVEL     4   /* 音标点阵级（40px） */

/* 刷新档：0=GC16 全刷；1=DU 全屏窗口；2=DU 右栏窗口 */
static int s_flush_mode = 0;

static int s_mean_page = 0;
static int s_last_word = -2;
static bool s_last_reveal = true;
static bool s_last_collected = false;
static study_mode_t s_last_mode = (study_mode_t)-1;
static bool s_first_render = true;

static int mean_pages(const char *meaning, int max_w)
{
    if (!meaning || !meaning[0]) return 1;
    int lines = cjk_text_wrap_lines(max_w, MEAN_LEVEL, meaning);
    if (lines <= 0) return 1;
    return (lines + MEAN_MAX_LINES - 1) / MEAN_MAX_LINES;
}

bool word_card_ui_mean_page_step(int dir)
{
    const WordEntry *w = word_parser_get(study_mode_current_word_index());
    if (!w || !w->meaning[0] || study_mode_is_revealed() == false) {
        /* 无释义/遮蔽中无词内翻页语义（遮蔽块无页） */
        return false;
    }
    int pages = mean_pages(w->meaning, RIGHT_W);
    int next = s_mean_page + dir;
    if (next < 0 || next >= pages) return false;   /* 边缘：交编排层翻词 */
    s_mean_page = next;
    word_card_ui_render();   /* 同词翻页：入口自动判右栏 DU 档 */
    return true;
}

/* 右对齐画 ASCII 文本（FreeSans/Helv；返回宽度供链式排布） */
static int draw_text_right(int right_x, int baseline_y, const char *s,
                           int font_size, uint16_t color)
{
    int w = 0, h = 0;
    epd_gfx_text_bounds(s, font_size, &w, &h);
    epd_gfx_draw_text(right_x - w, baseline_y, s, color, font_size);
    return w;
}

/* 尾部刷新分档：见文件头「刷新三档」（2=右栏窗口，1=全屏 DU 窗口，0=GC16 全刷） */
static void flush_tail(void)
{
    switch (s_flush_mode) {
    case 2:
        epd_gfx_flush_window(RIGHT_X, TOP_H, RIGHT_W,
                             epd_gfx_height() - TOP_H - BOTTOM_H);
        break;
    case 1:
        epd_gfx_flush_window(0, 0, epd_gfx_width(), epd_gfx_height());
        break;
    default:
        epd_gfx_flush();
        break;
    }
}

/* 底栏键位提示（静态文案：按键编排 app_main.c V2.1 大屏版核心操作；
 * RST 首词/错词本等次级操作不上带，保持单行可读） */
static void draw_bottom_bar(void)
{
    static const char hint[] =
        "上下 释义/翻词 · SET 揭晓 · 左右 忘了/简单 · 长按中键 菜单";
    int W = epd_gfx_width();
    cjk_text_draw((W - cjk_text_width(LBL_LEVEL, hint)) / 2,
                  epd_gfx_height() - BOTTOM_H +
                      (BOTTOM_H - cjk_glyph_cell_size(LBL_LEVEL)) / 2,
                  LBL_LEVEL, hint, EPD_GFX_BLACK);
}

void word_card_ui_render(void)
{
    int W = epd_gfx_width(), H = epd_gfx_height();

    study_mode_t m = study_mode_current();
    int wi = study_mode_current_word_index();
    const WordEntry *w = word_parser_get(wi);
    bool reveal = study_mode_is_revealed();
    bool collected = (w && wi >= 0) && learning_state_is_collected(wi);

    /* 刷新判档：首帧/模式切换 = 全刷；换词/收藏★变化（顶栏徽标 +
     * 左栏词头均有变）= 全屏窗口 DU；同词揭晓/释义翻页 = 右栏窗口
     * DU（左栏词头/顶栏/底栏逐字节不变，DU 差异行自动跳过） */
    if (s_first_render || m != s_last_mode) {
        s_flush_mode = 0;
    } else if (wi != s_last_word || collected != s_last_collected) {
        s_flush_mode = 1;
    } else {
        s_flush_mode = 2;
    }

    LOG_I("词卡渲染：wi=%d, total=%d, flush_mode=%d, first=%d",
          wi, study_mode_seq_total(), s_flush_mode, s_first_render);
    s_first_render = false;
    s_last_mode = m;
    s_last_collected = collected;

    /* 词/遮蔽态变化 → 释义页码归零（新词从第一页看起） */
    if (wi != s_last_word || reveal != s_last_reveal) {
        s_mean_page = 0;
        s_last_word = wi;
        s_last_reveal = reveal;
    }

    epd_gfx_fill_screen(EPD_GFX_WHITE);

    /* ---- 顶栏（白底黑字：模式名 + 徽标 + 序号） ---- */
    cjk_text_draw(PAD, (TOP_H - cjk_glyph_cell_size(LBL_LEVEL)) / 2,
                  LBL_LEVEL, study_mode_name(m), EPD_GFX_BLACK);

    int pos = study_mode_seq_pos(), total = study_mode_seq_total();
    char buf[48];

    /* 收藏★ / 墨封标记（序号左侧；墨封词不进闪卡序列，仅临时视图
     * 与刚置位瞬间可见——置位后 after_master 收缩，此标记主要服务
     * 墨封录/收藏视图内自证） */
    int rx = W - PAD;
    snprintf(buf, sizeof(buf), "%d/%d", pos + 1, total);
    rx -= draw_text_right(rx, TOP_H / 2 + 8, buf, 2, EPD_GFX_BLACK) + 40;
    if (w && wi >= 0) {
        if (learning_state_is_mastered(wi)) {
            int mw = cjk_text_width(LBL_LEVEL, "墨封");
            cjk_text_draw(rx - mw, (TOP_H - cjk_glyph_cell_size(LBL_LEVEL)) / 2,
                          LBL_LEVEL, "墨封", EPD_GFX_BLACK);
            rx -= mw + 40;
        }
        if (collected) {
            /* FreeSans '*' 近似星标（点阵星形字符未收录） */
            epd_gfx_set_bold(true);
            rx -= draw_text_right(rx, TOP_H / 2 + 8, "*", 2,
                                  EPD_GFX_BLACK) + 24;
            epd_gfx_set_bold(false);
        }
    }

    /* ---- 主内容 ---- */
    if (!w) {
        /* 空态页（空词库/序列清空：全部墨封、今日无到期词等）：
         * 保留顶栏/底栏框架，主区居中提示 */
        const char *msg = "序列为空";
        const char *hint = total <= 0 ? "词库空或当前视图无词条"
                                      : "按 RST 短按回首";
        cjk_text_draw((W - cjk_text_width(MEAN_LEVEL, msg)) / 2,
                      H / 2 - cjk_glyph_cell_size(MEAN_LEVEL),
                      MEAN_LEVEL, msg, EPD_GFX_BLACK);
        cjk_text_draw((W - cjk_text_width(LBL_LEVEL, hint)) / 2,
                      H / 2 + 40, LBL_LEVEL, hint, EPD_GFX_BLACK);
        draw_bottom_bar();
        flush_tail();
        return;
    }

    /* ---- 左栏（白底黑字：词头/音标/徽标） ---- */
    epd_gfx_draw_vline(LEFT_W, TOP_H, H - TOP_H - BOTTOM_H, EPD_GFX_BLACK);

    /* 词头：48pt Bold 起，超左栏宽限（640-2*80=480px）逐档降
     * （48→36→24pt；bounds 随 s_bold 走 bold 表，检测须在 bold 态） */
    epd_gfx_set_bold(true);
    int head_font = 6;
    while (head_font > 4) {
        int tw = 0, th = 0;
        epd_gfx_text_bounds(w->text, head_font, &tw, &th);
        if (tw <= LEFT_W - 2 * PAD) break;
        head_font--;
    }
    epd_gfx_draw_text(PAD, HEAD_BASE_Y, w->text, EPD_GFX_BLACK, head_font);
    epd_gfx_set_bold(false);

    /* 音标：40px 点阵黑字（裸音标自动补 / /） */
    if (w->phonetic[0]) {
        char ph[WORD_PHONETIC_MAX + 4];
        if (w->phonetic[0] == '/') {
            snprintf(ph, sizeof(ph), "%s", w->phonetic);
        } else {
            snprintf(ph, sizeof(ph), "/%s/", w->phonetic);
        }
        cjk_text_draw(PAD, PHON_Y, PHON_LEVEL, ph, EPD_GFX_BLACK);
    }

    /* 左栏徽标行：tag·grade·source（中点·收录集内，20px 黑字） */
    char lbl[WORD_TAG_MAX + WORD_GRADE_MAX + WORD_SOURCE_MAX + 8];
    lbl[0] = '\0';
    if (w->tag[0]) strncat(lbl, w->tag, sizeof(lbl) - strlen(lbl) - 1);
    if (w->grade[0]) {
        if (lbl[0]) strncat(lbl, "·", sizeof(lbl) - strlen(lbl) - 1);
        strncat(lbl, w->grade, sizeof(lbl) - strlen(lbl) - 1);
    }
    if (w->source[0]) {
        if (lbl[0]) strncat(lbl, "·", sizeof(lbl) - strlen(lbl) - 1);
        strncat(lbl, w->source, sizeof(lbl) - strlen(lbl) - 1);
    }
    if (lbl[0])
        cjk_text_draw(PAD, BADGE_Y, LBL_LEVEL, lbl, EPD_GFX_BLACK);

    /* ---- 右栏（白底黑字：释义遮蔽块 / 揭晓多页） ---- */
    if (!reveal) {
        /* 遮蔽：黑块 + 白字提示（自测：看词回忆释义） */
        epd_gfx_fill_rect(RIGHT_X, MEAN_Y0, RIGHT_W,
                          MEAN_MAX_LINES * MEAN_LINE_H, EPD_GFX_BLACK);
        const char *hint = "释义已遮蔽 · 短按 SET 揭晓";
        cjk_text_draw(RIGHT_X + (RIGHT_W - cjk_text_width(LBL_LEVEL, hint)) / 2,
                      MEAN_Y0 + (MEAN_MAX_LINES * MEAN_LINE_H -
                                 cjk_glyph_cell_size(LBL_LEVEL)) / 2,
                      LBL_LEVEL, hint, EPD_GFX_WHITE);
    } else if (w->meaning[0]) {
        int drawn = cjk_text_draw_wrap_page(
            RIGHT_X, MEAN_Y0, RIGHT_W, MEAN_LEVEL, MEAN_LINE_H,
            MEAN_MAX_LINES, s_mean_page, w->meaning, EPD_GFX_BLACK);
        (void)drawn;
        /* 多页指示（右栏右下） */
        int pages = mean_pages(w->meaning, RIGHT_W);
        if (pages > 1) {
            snprintf(buf, sizeof(buf), "释义 %d/%d 页", s_mean_page + 1,
                     pages);
            cjk_text_draw(RIGHT_X + RIGHT_W - cjk_text_width(LBL_LEVEL, buf),
                          H - BOTTOM_H - 44, LBL_LEVEL, buf, EPD_GFX_BLACK);
        }
    }

    draw_bottom_bar();

    LOG_D("render: mode=%s word=%s reveal=%d page=%d flush=%d",
          study_mode_name(m), w->text, reveal, s_mean_page, s_flush_mode);
    flush_tail();
}
