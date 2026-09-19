/**
 * @file standby_page.c
 * @brief 大屏待机页实现 —— 学习统计仪表盘 + 引文（2026-09-17 UI 重设计）
 *
 * 仪表盘三带（墨水屏宁静属性优先：统计作上下带，引文居中为视觉主体）：
 *   上带 y 140：今日统计「新词 N · 复习 M」（左）+「连续 X 天」（右），
 *        40px 点阵；y 240 细线收边
 *   中央：引文 48px 楷体（逐时轮换——RTC 同步前 esp_timer 开机小时
 *        取模，小屏同款降级语义；\n 分行每行 <=8 字），行距 84 垂直
 *        居中；出处跟随引文块右下署名 20px（阅读动线优于屏角）
 *   下带 y 740 细线起：学习进度条（DASH_PAD 收口，2px 外框 + 比例
 *        填充；已学 = 词库 - 未学未墨封）+「已学 N / M 词」（左）
 *        +「墨封·收藏·错词」徽标（右），32px
 *
 * 全屏白底黑字 + GC16 全刷（低频页：5 分钟待机进入时渲染一次）。
 */
#include "standby_page.h"

#include <stdio.h>
#include <string.h>

#include "cjk_font.h"
#include "cjk_text.h"
#include "epd_gfx.h"
#include "esp_timer.h"
#include "learning_state.h"
#include "quotes_app.h"
#include "word_parser.h"

#define STAT_LEVEL    4   /* 上带今日统计 40px */
#define QUOTE_LEVEL   5   /* 引文 48px（楷体级） */
#define PROG_LEVEL    3   /* 下带进度/徽标 32px */
#define LBL_LEVEL     1   /* 出处署名 20px */
#define DASH_PAD     160  /* 仪表盘左右收口 */
#define LINE1_Y      240  /* 上带下界线 */
#define LINE2_Y      740  /* 下带上界线 */
#define BAR_Y        820  /* 进度条顶 */
#define BAR_H         36  /* 进度条高 */

/* \n 分行渲染（引文表约定每行 <=8 字，不再二次断行） */
static int split_lines(const char *s, const char *lines[], int max)
{
    int n = 0;
    const char *p = s;
    while (p && *p && n < max) {
        const char *nl = strchr(p, '\n');
        lines[n++] = p;
        if (!nl) break;
        p = nl + 1;
    }
    return n;
}

int64_t standby_time_now(void)
{
    /* 自治钟：esp_timer 单调秒（learning_state 今日统计/评分时基） */
    return esp_timer_get_time() / 1000000;
}

void standby_page_render(void)
{
    int W = epd_gfx_width(), H = epd_gfx_height();
    char buf[64];

    epd_gfx_fill_screen(EPD_GFX_WHITE);

    /* ---- 上带：今日统计（左）+ 连续天数（右） ---- */
    snprintf(buf, sizeof(buf), "新词 %d · 复习 %d",
             learning_state_today_new(), learning_state_today_reviews());
    cjk_text_draw(DASH_PAD, 140, STAT_LEVEL, buf, EPD_GFX_BLACK);

    snprintf(buf, sizeof(buf), "连续 %d 天", learning_state_streak_days());
    cjk_text_draw(W - DASH_PAD - cjk_text_width(STAT_LEVEL, buf), 140,
                  STAT_LEVEL, buf, EPD_GFX_BLACK);

    epd_gfx_draw_hline(DASH_PAD, LINE1_Y, W - 2 * DASH_PAD, EPD_GFX_BLACK);

    /* ---- 中央：引文（逐时轮换，48px 楷体居中） ---- */
    int hour = (int)(esp_timer_get_time() / 1000000LL / 3600);
    const char *quote = k_chuanxilu_quotes[hour % CHUANXILU_QUOTE_N];

    const char *lines[8];
    int n = split_lines(quote, lines, 8);
    int cell = cjk_glyph_cell_size(QUOTE_LEVEL);
    int line_h = cell + 36;
    int block_h = n * line_h;
    int y = (H - block_h) / 2;
    for (int i = 0; i < n; i++) {
        /* 分行长上限（8 字 + NUL；strchr 定行长画前缀） */
        char tmp[64];
        const char *nl = strchr(lines[i], '\n');
        size_t len = nl ? (size_t)(nl - lines[i]) : strlen(lines[i]);
        if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
        memcpy(tmp, lines[i], len);
        tmp[len] = '\0';
        int w = cjk_text_width(QUOTE_LEVEL, tmp);
        cjk_text_draw((W - w) / 2, y, QUOTE_LEVEL, tmp, EPD_GFX_BLACK);
        y += line_h;
    }

    /* 出处：引文块右下跟随署名 */
    int aw = cjk_text_width(LBL_LEVEL, k_chuanxilu_attrib);
    int acell = cjk_glyph_cell_size(LBL_LEVEL);
    cjk_text_draw(W - DASH_PAD - aw, (H + block_h) / 2 + 24, LBL_LEVEL,
                  k_chuanxilu_attrib, EPD_GFX_BLACK);

    /* ---- 下带：进度条 + 进度文本（左）+ 徽标（右） ---- */
    epd_gfx_draw_hline(DASH_PAD, LINE2_Y, W - 2 * DASH_PAD, EPD_GFX_BLACK);

    /* 已学 = 词库总量 - 未学且未墨封（墨封词必然学过，计入已学） */
    int total = word_parser_get_count();
    int learned = total - learning_state_active_new_count();
    if (learned < 0) learned = 0;
    if (learned > total) learned = total;

    int bar_w = W - 2 * DASH_PAD;
    epd_gfx_fill_rect(DASH_PAD, BAR_Y, bar_w, 2, EPD_GFX_BLACK);
    epd_gfx_fill_rect(DASH_PAD, BAR_Y + BAR_H - 2, bar_w, 2, EPD_GFX_BLACK);
    epd_gfx_fill_rect(DASH_PAD, BAR_Y, 2, BAR_H, EPD_GFX_BLACK);
    epd_gfx_fill_rect(DASH_PAD + bar_w - 2, BAR_Y, 2, BAR_H, EPD_GFX_BLACK);
    if (total > 0 && learned > 0) {
        int fill_w = (bar_w - 8) * learned / total;   /* 内缩 4px 呼吸边 */
        if (fill_w > 0)
            epd_gfx_fill_rect(DASH_PAD + 4, BAR_Y + 4, fill_w,
                              BAR_H - 8, EPD_GFX_BLACK);
    }

    snprintf(buf, sizeof(buf), "已学 %d / %d 词", learned, total);
    cjk_text_draw(DASH_PAD, BAR_Y + BAR_H + 28, PROG_LEVEL, buf,
                  EPD_GFX_BLACK);

    snprintf(buf, sizeof(buf), "墨封 %d · 收藏 %d · 错词 %d",
             learning_state_mastered_count(),
             learning_state_collected_count(),
             learning_state_wrong_count());
    cjk_text_draw(W - DASH_PAD - cjk_text_width(PROG_LEVEL, buf),
                  BAR_Y + BAR_H + 28, PROG_LEVEL, buf, EPD_GFX_BLACK);

    epd_gfx_flush();
}
