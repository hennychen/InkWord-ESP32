/**
 * @file word_card_ui.cpp
 * @brief 单词卡片/学习页渲染族实现（P2 巨石拆分：自 main.cpp 迁出，
 *        行为零变化——黄金帧基线不受影响）
 *
 * 自 main.cpp 迁入的静态状态（单一定义点）：
 *   - s_last_mode：模式切换全刷判定（ui_force_* 置无效值）
 *   - s_mean_page/s_mean_word：释义分页游标（与词绑定）
 */
#include "word_card_ui.h"

#include "epd_driver.h"          /* epd_gfx_* / epd_set_rotation /
                                  * epd_panel_default_rotation */
#include "study_mode_machine.h"
#include "word_parser.h"         /* WordEntry / word_parser_get */
#include "cjk_text.h"            /* 释义/tag 中文点阵混排 */
#include "layout_profile.h"      /* UI_* 档位派生源 */
#include "card_layout.h"         /* qa/poem 版式分派 */
#include "deck_manager.h"        /* ui_card_layout: active_payload_type */
#include "learning_state.h"      /* is_collected */
#include "daily_plan.h"         /* 复习空态问候页判据 */
#include "settings_ui.h"         /* settings_font_mode/word_size/rot */
#include "review_ui.h"           /* MODE_REVIEW 列表/详情态 */
#include "reader_engine.h"       /* MODE_READER 渲染 */
#include "standby_page.h"        /* 词库空待机页 / invalidate_layout */
#include "quiz_ui.h"             /* MODE_QUIZ 绘制 */
#include "ui_stamp.h"            /* 墨封「熟」角标（2026-09-04） */
#include "page_router.h"         /* display_busy 守卫 / render_top */
#include "refresh_scheduler.h"   /* refresh_gfx_before_partial */
#include "debug_log.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "WORD_UI";

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
#define UI_STATUS_H     (layout_profile_get()->status_h)  /* 状态栏高度（内容区顶 y；T1.5 档位参数表） */
#define UI_MARGIN_X     (layout_profile_get()->margin_x)   /* 左右留白（T1.5 档位参数表） */
#define UI_STATUS_BASE  (UI_STATUS_H - 10)             /* 状态栏文字基线（22/14） */
/* ---- 学习页单列版式（全档位统一，2026-08-23 重设计）----
 * 上下结构：头部单词（全宽大字自适应）+ 音标 + 收藏星标；正文流 =
 * 释义+词根全宽分页；底部标签行全宽。双栏版式退役（真机反馈 136px
 * 右栏 7 字/行阅读体验差，416x240 全宽 21 字/行提升 3 倍）；
 * 字号档位派生：TINY/SMALL 16px / MID+ 20px */
#define UI_MEAN_LEVEL   (layout_profile_get()->kind <= LAYOUT_SMALL \
                         ? (settings_font_mode() >= 1 ? 1 \
                            : (layout_profile_get()->narrow_tiny ? 1 : 0)) \
                         : (layout_profile_get()->mean_level >= 3 ? 3 \
                            : layout_profile_get()->mean_level \
                              + (settings_font_mode() >= 1 ? 1 : 0)))  /* 正文字号级：
 * 档位默认 TINY/SMALL 16px；MID+ 默认读 mean_level（2026-09-08 PPI
 * 自动层 3.4mm 目标：MID 20px / LARGE 32px / 7.5"@150 24px），
 * 大字/特大档（set_font>=1）整体 +1 级、LARGE 封顶 3，
 * 行距与几何全部由本宏派生自适应。2026-08-27
 * P1a 三档化：意图相对档位表达（set_rot 同哲学）——TINY 屏宽 122~
 * 128px 下 24px 每行仅 3~4 字 / SMALL 横屏 176 高正文行数趋零，
 * 特大档(2)在 TINY/SMALL 钳位至 20px（渲染等价大字，语义不漂移）。
 * 2026-08-30 OPM021EB 真机补充：2.13" 122 宽（0.1943mm 像素距
 * ≈135DPI）16px 字物理仅 3.1mm，音标/释义/标签实测发虚看不清
 * （单词大字清晰对比佐证），默认提 20px；2.9" 128 宽（90DPI）
 * 保持 16px（WFT0290 真机验收通过口径） */
#define UI_WORD_BASE    (UI_STATUS_H + (UI_TINY ? 24 \
                         : (UI_MEAN_LEVEL ? 36 : 32)))  /* 单词基线（68/64/48） */
#define UI_PHON_TOP     (UI_WORD_BASE + (UI_TINY ? 6 : 9))     /* 音标行 16px 点阵顶（77/73/54） */
#define UI_BODY_TOP     (UI_PHON_TOP + (UI_AUX_LEVEL ? 20 : 16) + (UI_TINY ? 4 : 11)) /* 正文流首行顶：音标行高随辅助级 */
#define UI_BODY_LH      (UI_TINY ? (UI_MEAN_LEVEL ? 26 : 20) \
                         : (UI_MEAN_LEVEL >= 3 ? 36 : (UI_MEAN_LEVEL ? 24 : 20)))  /* 正文行距：字级 +4（reader 惯例，LARGE 32px 级 2026-09-08 扩 36）；二十四轮（2026-08-31）TINY 档 20px 级膨胀加粗后 24→26：笔画变粗视觉更满，行间空隙 4→6px 防粘连（低对比度屏稀疏化），每页行数 6→5 */
#define UI_AUX_LEVEL    (layout_profile_get()->narrow_tiny ? UI_MEAN_LEVEL : 0)
                                   /* 辅助小字级（音标/标签行）：
 * 2.13" 122 宽（135DPI）跟随正文级（20px），其余屏（含 2.9" 128
 * 宽）保持 16px 原口径。2026-08-30 OPM021EB 真机：16px 音标/标签
 * 与单词大字清晰度对比悬殊，主诉“显示不清” */
/* 行数按屏高派生：底部预留 30 = 标签行 + 余量（末行文字底与标签顶
 * 错开，MID 末行底 196 < 标签顶 206）；416x240=4 行、400x300=6、
 * 264x176=2、122x250 竖屏=7、128x296 竖屏=9（TINY 预留收至 26） */
#define UI_BODY_RESERVE (layout_profile_get()->body_reserve)
#define UI_BODY_LINES_  ((epd_gfx_height() - UI_BODY_RESERVE - UI_BODY_TOP) / UI_BODY_LH)
#define UI_BODY_LINES   (UI_BODY_LINES_ < 1 ? 1 : UI_BODY_LINES_)  /* 下限 1：
 * 极端几何（窄屏高字号叠加）防御，正文区至少 1 行可翻页（P1a） */
#define UI_BODY_MAX_W   (epd_gfx_width() - 2 * UI_MARGIN_X)   /* 全宽正文（392/232/106/112） */
#define UI_FOOT_BASE    (epd_gfx_height() - 16)        /* 底部标签基线：底边距 16（224） */
#define UI_FOOT_TOP     (UI_FOOT_BASE - (UI_AUX_LEVEL ? 22 : 18)) /* tag 点阵顶：字高+2（208/206） */
/* ---- v1.4 T4.3 poem-card 头部几何：诗行（正文字号大一级，绝句两句
 * 内）+ 拼音行（16px），译文区从拼音行下 8px 起（ui_mean_geom 派生）；
 * MID 默认档 124 起可容 5 行译文，SMALL 1 行/页分页翻 ---- */
#define UI_POEM_LEVEL     (UI_MEAN_LEVEL < 2 ? UI_MEAN_LEVEL + 1 : 2)
#define UI_POEM_LH        (UI_POEM_LEVEL * 4 + 20)     /* 22/26/30：字级+6 呼吸感 */
#define UI_POEM_LINES     2                            /* 诗行上限（超行截断，T4.4 约定存精华句） */
#define UI_POEM_TOP       (UI_STATUS_H + 8)
#define UI_POEM_PIN_TOP   (UI_POEM_TOP + UI_POEM_LINES * UI_POEM_LH + 4)
#define UI_POEM_TRANS_TOP (UI_POEM_PIN_TOP + 16 + 8)   /* 拼音行底 + 8 */

/* 模式切换全刷判定（无效值=强制全刷；ui_force_* 置位） */
static study_mode_t s_last_mode = MODE_COUNT;

/* 音标行 16px 点阵整行渲染（2026-08-23 IPA 修复，08-24 记号补全）：
 * 原方案 FreeSans（仅 0x20-0x7E）逐字跳过非 ASCII——真机 'ˈizi'
 * 重音符丢失，曾以 phonetic_ascii 转 ASCII 近似（ə→e 发音错位）；
 * 现字库收录 IPA 21 字符（STHeitiSC-Medium 点阵，gen_cjk_font.swift
 * 分派渲染）+ 诗词词条作者名/中点（default_words.json phonetic 列
 * 全量收集），词池原始 IPA 直渲。08-24 补全：① 词典惯例斜杠包裹
 * ——词库 phonetic 为裸 IPA（无 / /），显示层条件补齐（自带 / 或 [ ]
 * 的云端/SD 词库不双包）；② ˈ ˌ ː · 四记号生成器合成位图（字体
 * 渲染 16px 级 1px 细笔低于阈值被丢弃，重音符曾显为空格）；
 * 未收录字符画 cell 空心框兜底（定义上移至快速测验块前） */

/* 释义分页游标（2026-08-23）：与词绑定——换词/换模式（含错词本进出、
 * RST 回首、自评移词）给 ui_render_word 检测到词变化即归零，同词
 * SET 翻义保持页位；页数由排版几何实时派生（见 ui_mean_total_pages） */
static int s_mean_page = 0;      /* 当前释义页（0 基） */
static int s_mean_word = -1;     /* 页游标绑定的词库索引（错词本=映射后） */

/* 单词字号偏好（set_word）→ fit 起步档：0=大(24pt)/1=中(18pt)/2=小(14pt)；
 * 超宽自动降级机制不变（ui_fit_font 向下遍历）；默认 0 = 历史行为
 * start 4，视觉零变化（2026-08-27 P1b）。 */
int ui_word_start_size(void)
{
    int w = settings_word_size();
    return w == 2 ? 2 : (w == 1 ? 3 : 4);
}

/* 字号自适应：从 start_size 逐级降到能放进 max_w 的字号 */
int ui_fit_font(const char *text, int start_size, int max_w)
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

/* 释义正文流（全档位单列）：释义 + 全角空格(U+3000) + 词根 + 例句
 * （单列无独立槽位，随释义滚动分页；空段前导空白被断行核心
 * 行首吞掉。v1.2 T2.7：例句入流——静态核验确认 ui_draw_content
 * 此前不消费 example，按任务补在词根后，阅读顺序即「词根下方」） */
static const char *ui_body_stream(const WordEntry *w)
{
    if (!w->root[0] && !w->example[0])
        return w->meaning;
    static char stream[WORD_MEANING_MAX + WORD_ROOT_MAX +
                       WORD_EXAMPLE_MAX + 12];
    if (!w->example[0])
        snprintf(stream, sizeof(stream), "%s\xE3\x80\x80%s",
                 w->meaning, w->root);
    else if (!w->root[0])
        snprintf(stream, sizeof(stream), "%s\xE3\x80\x80%s",
                 w->meaning, w->example);
    else
        snprintf(stream, sizeof(stream), "%s\xE3\x80\x80%s\xE3\x80\x80%s",
                 w->meaning, w->root, w->example);
    return stream;
}

/* 页数 = 总行数向上取整 / 每页行数（空文 1 页兜底） */
static int ui_page_count(const char *s, int max_w, int lines)
{
    int need = cjk_text_wrap_lines(max_w, UI_MEAN_LEVEL, s);
    int pages = (need + lines - 1) / lines;
    return pages < 1 ? 1 : pages;
}

/* v1.4 T4.3：当前卡组版式（渲染层唯一分派点，card_layout.c 映射） */
static card_layout_t ui_card_layout(void)
{
    return card_layout_from_payload(deck_manager_active_payload_type());
}

/* qa 题干行数：量测值 clamp 至正文行数一半（答案区保障半屏；
 * 量测与绘制同源——ui_mean_geom 与 ui_draw_content_qa 共用） */
static int ui_qa_stem_lines(const WordEntry *w)
{
    int total = cjk_text_wrap_lines(UI_BODY_MAX_W, UI_MEAN_LEVEL, w->text);
    int cap = UI_BODY_LINES / 2;
    if (cap < 1) cap = 1;
    return total < 1 ? 1 : (total > cap ? cap : total);
}

/* 释义区几何（单列统一，档位字号派生）：全宽正文流；strip>0 表示
 * 多页时页码指示与正文首行同行，正文右侧须预留指示条并按缩窄宽度
 * 重建页数（量测与绘制同宽，断行一致）；ind_* = 指示器右缘 x / 基线 y */
static void ui_mean_geom(const WordEntry *w, card_layout_t layout,
                         int *x, int *top, int *max_w, int *lh, int *lines,
                         int *ind_rx, int *ind_by, int *strip)
{
    int n = UI_BODY_LINES;
    if (n < 1) n = 1;
    *x = UI_MARGIN_X; *top = UI_BODY_TOP;
    *max_w = UI_BODY_MAX_W; *lh = UI_BODY_LH;
    *lines = n;
    /* v1.4 T4.3 版式派生：qa 答案区让出题干行（+1/3 行距），poem
     * 译文区让出诗行+拼音头部；行数按剩余高度重建（至少 1 行保底） */
    if (layout == CARD_LAYOUT_QA) {
        *top += ui_qa_stem_lines(w) * UI_BODY_LH + UI_BODY_LH / 3;
        *lines = (UI_FOOT_TOP - 6 - *top) / *lh;
        if (*lines < 1) *lines = 1;
    } else if (layout == CARD_LAYOUT_POEM) {
        *top = UI_POEM_TRANS_TOP;
        *lines = (UI_FOOT_TOP - 6 - *top) / *lh;
        if (*lines < 1) *lines = 1;
    }
    *ind_rx = epd_gfx_width() - UI_MARGIN_X;
    *ind_by = *top + (UI_MEAN_LEVEL ? 20 : 14); /* 正文首行基线（右缘同行） */
    /* 多页页码指示条预留：TINY 加宽（106px 正文下每行仅 3~4 字，
     * 页数易破十→“10/11” 5 字符 size1 ≈40px>30 会压首行末字） */
    *strip = UI_TINY ? 44 : 30;
}

/* 当前布局下释义流总页数（含 SMALL 指示条缩窄重建，与绘制同口径） */
static int ui_mean_total_pages(const WordEntry *w)
{
    int x, top, max_w, lh, lines, ind_rx, ind_by, strip;
    ui_mean_geom(w, ui_card_layout(), &x, &top, &max_w, &lh, &lines,
                 &ind_rx, &ind_by, &strip);
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
    ui_mean_geom(w, ui_card_layout(), &x, &top, &max_w, &lh, &lines,
                 &ind_rx, &ind_by, &strip);
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
bool ui_mean_page_step(int dir)
{
    if (study_mode_current() == MODE_READER) return false;
    if (!study_mode_is_revealed()) return false;   /* 遮蔽自测态无页可翻 */

    const WordEntry *w = word_parser_get(study_mode_current_word_index());
    if (!w) return false;

    int np = s_mean_page + dir;
    if (np < 0 || np >= ui_mean_total_pages(w)) return false;
    s_mean_page = np;
    page_router_render_top();   /* 经 base 分流重绘（无覆盖层时） */
    return true;
}

/* 绘制状态栏：模式名（左）+ 序号（右，错词本=序号/错词数）+ 分隔线
 * （T2.2 导出：chat_mode 首帧 chat_page_enter 取用） */
void ui_draw_status(study_mode_t mode)
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
                               /*level*/UI_AUX_LEVEL, 0, 1, foot, EPD_GFX_BLACK);
        else
            epd_gfx_draw_text(UI_MARGIN_X, UI_FOOT_BASE, foot,
                              EPD_GFX_BLACK, 1);
    }
}

/* ---- 遮蔽态视图（2026-08-24 重设计，百词斩借鉴）---- */

/* 听写遮蔽态判定：听写本质=听音忆拼写，遮蔽期藏单词与音标（显示
 * 即泄题），头部改画首字母+拼写空格线（百词斩拼写填空同款视觉） */
static bool ui_dict_blind(void)
{
    return study_mode_current() == MODE_DICTATION && !study_mode_is_revealed();
}

/* 拼写空格线：首字母实显 + 每剩余字母位底线段（填空式视觉锚点，
 * 底线数=拼写长度线索）；位距按档位派生，超宽截位（TINY 106px
 * ≈ 8 位——截位丢长度线索，bring-up 后再调；MID 17 位全覆盖） */
static void ui_draw_spelling_slots(const char *word)
{
    int len = (int)strlen(word);
    int pitch  = UI_TINY ? 13 : 22;    /* 位距（含间隙） */
    int slot_w = UI_TINY ? 10 : 16;    /* 底线段宽 */
    int max_slots = (UI_BODY_MAX_W - 16) / pitch;
    if (max_slots < 2) max_slots = 2;
    if (len > max_slots) len = max_slots;

    int y_top = UI_STATUS_H + 8;       /* 字母行顶（近似单词行视觉位） */
    char c[2] = { word[0], 0 };
    cjk_text_draw(UI_MARGIN_X, y_top, 0, c, EPD_GFX_BLACK);
    for (int i = 1; i < len; i++)
        epd_gfx_draw_hline(UI_MARGIN_X + i * pitch, y_top + 18, slot_w,
                           EPD_GFX_BLACK);
}

/* 遮蔽态正文：闪卡族（闪卡/复习详情/错词本/收藏）居中大问号 +
 * 中文揭晓提示（自测仪式感，取代英文 "[SET] to reveal"）；
 * 听写头部已是空格线，正文只画重播提示 */
static void ui_draw_hidden_body(void)
{
    if (ui_dict_blind()) {
        cjk_text_draw(UI_MARGIN_X, UI_BODY_TOP, UI_MEAN_LEVEL,
                      "中键重播 · SET 揭晓", EPD_GFX_BLACK);
        return;
    }

    int qh = UI_MEAN_LEVEL ? 20 : 16;             /* 全角问号字号级 */
    int qw = cjk_text_width(UI_MEAN_LEVEL, "？");
    int avail = UI_FOOT_TOP - UI_BODY_TOP;        /* 正文区可用高 */
    int y_q = UI_BODY_TOP + (avail - qh - 30) / 2;
    if (y_q < UI_BODY_TOP) y_q = UI_BODY_TOP;
    cjk_text_draw((epd_gfx_width() - qw) / 2, y_q, UI_MEAN_LEVEL,
                  "？", EPD_GFX_BLACK);
    const char *hint = "[SET] 揭晓";
    int hw = cjk_text_width(0, hint);
    cjk_text_draw((epd_gfx_width() - hw) / 2, y_q + qh + 12, 0,
                  hint, EPD_GFX_BLACK);
}

/* ---- v1.4 T4.3 qa/poem 版式绘制（word 现版式零改动）----
 * 分派源 = 活跃卡组 manifest payloadType（card_layout.c 映射，缺省
 * word-card 英语开箱行为不变）；字段语义见 card_layout.h。遮蔽/
 * 揭晓、收藏、foot、正文分页全部复用 word 版基建（ui_mean_geom 按
 * 版式派生正文区几何，翻页链 ui_mean_page_step 自动同口径）。 */

/* qa 版式：题干多行 CJK 常驻正文区顶，答案（meaning 流）揭晓后分页；
 * 听写模式下即题面/答案天然遮蔽对（拼写空格线为英语专用不适用） */
static void ui_draw_content_qa(const WordEntry *w)
{
    cjk_text_draw_wrap(UI_MARGIN_X, UI_BODY_TOP, UI_BODY_MAX_W, UI_MEAN_LEVEL,
                       UI_BODY_LH, ui_qa_stem_lines(w), w->text, EPD_GFX_BLACK);

    /* 收藏星标 + 墨封角标：无头部行，画在正文区右上角（词卡音标行
     * 右缘同语义；墨封方印在 * 左侧，右缘锚点依次左移拼排） */
    {
        int wi = study_mode_current_word_index();
        int rx = epd_gfx_width() - UI_MARGIN_X;
        if (learning_state_is_collected(wi)) {
            int sw = cjk_text_width(0, "*");
            rx -= sw;
            cjk_text_draw(rx, UI_STATUS_H + 6, 0, "*", EPD_GFX_BLACK);
        }
        if (learning_state_is_mastered(wi))
            ui_draw_seal_mark(rx - 4, UI_STATUS_H + 6);
    }

    if (study_mode_is_revealed()) {
        ui_draw_mean_paged(w);
    } else {
        /* 答案遮蔽：答案区首行位提示（题干常驻，与居中问号态互斥） */
        int x, top, max_w, lh, lines, ind_rx, ind_by, strip;
        ui_mean_geom(w, CARD_LAYOUT_QA, &x, &top, &max_w, &lh, &lines,
                     &ind_rx, &ind_by, &strip);
        cjk_text_draw(x, top, 0, "[SET] 揭晓答案", EPD_GFX_BLACK);
    }

    ui_draw_foot(w, UI_BODY_MAX_W);
}

/* poem 头部：诗行（正文字号大一级）+ 拼音行（16px，音标渲染链复用） */
static void ui_draw_poem_head(const WordEntry *w)
{
    cjk_text_draw_wrap(UI_MARGIN_X, UI_POEM_TOP, UI_BODY_MAX_W, UI_POEM_LEVEL,
                       UI_POEM_LH, UI_POEM_LINES, w->text, EPD_GFX_BLACK);
    if (w->phonetic[0])
        cjk_text_draw_wrap(UI_MARGIN_X, UI_POEM_PIN_TOP, UI_BODY_MAX_W,
                           0, 16, 1, w->phonetic, EPD_GFX_BLACK);
}

/* poem 默写态 TINY 版（2026-08-25）：原框径随诗行字号（20px 框
 * pitch26，×5=130px）五言句仅容 3~4 框，字数线索断裂；改 16px
 * 小框 pitch20 单行 5 框（122/128px 正文宽均容）、七言分两行
 * （5+2）；上句降 16px 双行包裹（20px 下七言 140px 截尾丢题面）；
 * 揭示提示随框区下移动态定位 */
static void ui_draw_poem_dictation_tiny(const WordEntry *w)
{
    if (w->root[0])
        cjk_text_draw_wrap(UI_MARGIN_X, UI_POEM_TOP, UI_BODY_MAX_W,
                           0, 20, 2, w->root, EPD_GFX_BLACK);

    int slot = 16, pitch = 20;
    int per_row = (UI_BODY_MAX_W - 4) / pitch;   /* 122/128px 均 5 */
    if (per_row < 1) per_row = 1;

    int n = 0;
    for (const char *p = w->text; *p; ) {
        if ((*p & 0x80) == 0) p++;                        /* ASCII 罕见直跳 */
        else { n++; p += (*p & 0xE0) == 0xE0 ? 3 : 2; }   /* 全角占位 */
    }
    if (n > per_row * 3) n = per_row * 3;   /* 防御：题面契约 ≤7 字/句 */

    int y0 = UI_POEM_TOP + 2 * 20 + 4;      /* 上句双行占位后（保守固定） */
    for (int i = 0; i < n; i++)
        epd_gfx_draw_rect(UI_MARGIN_X + (i % per_row) * pitch,
                          y0 + (i / per_row) * (slot + 6),
                          slot, slot, EPD_GFX_BLACK);

    int y_hint = y0 + ((n + per_row - 1) / per_row) * (slot + 6) + 8;
    cjk_text_draw(UI_MARGIN_X, y_hint, 0, "[SET] 揭晓答案",
                  EPD_GFX_BLACK);
}

/* poem 默写态（T4.4，MODE_DICTATION + poem-card）：root=上句题面
 * 常驻首诗行位（数据契约：上句为单句），text=下句遮蔽画全角空框
 * （逐全角字符一方框，字数线索——百词斩拼写填空同构；框径=诗行
 * 字高，超宽截位同 spelling_slots 策略；TINY 档独立布局见上 _tiny
 * 版）；拼音行同遮（听写藏音标先例，防拼音泄底），揭晓后走正常
 * poem 版式（上下句均在屏） */
static void ui_draw_poem_dictation(const WordEntry *w)
{
    if (UI_TINY) { ui_draw_poem_dictation_tiny(w); return; }
    if (w->root[0])
        cjk_text_draw_wrap(UI_MARGIN_X, UI_POEM_TOP, UI_BODY_MAX_W,
                           UI_POEM_LEVEL, UI_POEM_LH, 1, w->root, EPD_GFX_BLACK);

    int slot = UI_POEM_LEVEL * 4 + 16;      /* 框径：16/20/24 同诗行字高 */
    int pitch = slot + 6;
    int max_n = (UI_BODY_MAX_W - 4) / pitch;
    if (max_n < 1) max_n = 1;

    int n = 0;
    for (const char *p = w->text; *p && n < max_n; ) {
        if ((*p & 0x80) == 0) p++;                        /* ASCII 罕见直跳 */
        else { n++; p += (*p & 0xE0) == 0xE0 ? 3 : 2; }   /* 全角占位 */
    }

    int y = UI_POEM_TOP + UI_POEM_LH + 4;   /* 次诗行位（上句单行契约） */
    for (int i = 0; i < n; i++)
        epd_gfx_draw_rect(UI_MARGIN_X + i * pitch, y, slot, slot, EPD_GFX_BLACK);

    cjk_text_draw(UI_MARGIN_X, UI_POEM_TRANS_TOP, 0,
                  "[SET] 揭晓答案", EPD_GFX_BLACK);
}

/* poem 版式：诗行+拼音头部常驻，译文（meaning 流）遮蔽→揭晓分页；
 * 遮蔽提示与 qa 同构（左对齐小字——头部已占屏上部，居中大问号
 * 在 SMALL 档会与拼音行重叠，见几何注）；默写态见 ui_draw_poem_dictation */
static void ui_draw_content_poem(const WordEntry *w)
{
    /* 收藏星标 + 墨封角标：拼音行右缘（遮蔽/默写态照画，word 版
     * 听写先例；墨封方印在 * 左侧，右缘锚点依次左移拼排） */
    {
        int wi = study_mode_current_word_index();
        int rx = epd_gfx_width() - UI_MARGIN_X;
        if (learning_state_is_collected(wi)) {
            int sw = cjk_text_width(0, "*");
            rx -= sw;
            cjk_text_draw(rx, UI_POEM_PIN_TOP, 0, "*", EPD_GFX_BLACK);
        }
        if (learning_state_is_mastered(wi))
            ui_draw_seal_mark(rx - 4, UI_POEM_PIN_TOP);
    }

    if (ui_dict_blind()) {
        ui_draw_poem_dictation(w);
        ui_draw_foot(w, UI_BODY_MAX_W);
        return;
    }

    ui_draw_poem_head(w);

    if (study_mode_is_revealed())
        ui_draw_mean_paged(w);
    else
        cjk_text_draw(UI_MARGIN_X, UI_POEM_TRANS_TOP, 0,
                      "[SET] 揭晓译文", EPD_GFX_BLACK);

    ui_draw_foot(w, UI_BODY_MAX_W);
}

/* 单列版式（全档位统一，2026-08-23 重设计）：头部单词（全宽大字
 * 自适应）+ 音标 + 收藏星标；正文流 = 释义+词根全宽分页；底部标签行 */
static void ui_draw_content(const WordEntry *w)
{
    epd_gfx_fill_rect(0, UI_STATUS_H, epd_gfx_width(),
                      epd_gfx_height() - UI_STATUS_H, EPD_GFX_WHITE);

    /* v1.4 T4.3 版式分派：qa/poem 各自绘制（含 foot），word 现版式零改动 */
    card_layout_t layout = ui_card_layout();
    if (layout == CARD_LAYOUT_QA)   { ui_draw_content_qa(w);   return; }
    if (layout == CARD_LAYOUT_POEM) { ui_draw_content_poem(w); return; }

    if (ui_dict_blind()) {
        ui_draw_spelling_slots(w->text);   /* 听写遮蔽：藏词画空格线 */
    } else if (cjk_text_has_wide(w->text)) {
        /* 2026-09-03 中文词条头（default_words.json 尾部高考古诗文：
         * text=诗题如「扬州慢（淮左名都）」）：FreeSans 仅 ASCII
         * 整行跳过→标题空白（用户实测）；含全角字符走点阵（音标行
         * 2026-08-23 同款路径分派，字库已收作者名/全角标点）。字级从
         * 诗行档（UI_POEM_LEVEL）超宽逐级降级，仍超宽单行截断（词头槽
         * 一行预算，TINY 106px 窄屏长诗题）；y=基线-cell 高，与 ASCII
         * 词底基线对齐近似 */
        int lvl = UI_POEM_LEVEL;
        while (lvl > 0 && cjk_text_width(lvl, w->text) > UI_BODY_MAX_W)
            lvl--;
        cjk_text_draw_wrap(UI_MARGIN_X, UI_WORD_BASE - (16 + lvl * 4),
                           UI_BODY_MAX_W, lvl, 16 + lvl * 4, 1,
                           w->text, EPD_GFX_BLACK);

        if (w->phonetic[0])
            /* 中文词条：作者名直显（署名非音标，不包斜杠） */
            cjk_text_draw(UI_MARGIN_X, UI_PHON_TOP, UI_AUX_LEVEL,
                          w->phonetic, EPD_GFX_BLACK);
    } else {
        epd_gfx_draw_text(UI_MARGIN_X, UI_WORD_BASE, w->text, EPD_GFX_BLACK,
                          ui_fit_font(w->text, ui_word_start_size(),
                                      UI_BODY_MAX_W));

        if (w->phonetic[0]) {
            /* 词典惯例斜杠包裹：裸 IPA 补 / /，自带包裹符（/[）不双包 */
            if (w->phonetic[0] == '/' || w->phonetic[0] == '[')
                cjk_text_draw(UI_MARGIN_X, UI_PHON_TOP, UI_AUX_LEVEL,
                              w->phonetic, EPD_GFX_BLACK);
            else {
                char ph[WORD_PHONETIC_MAX + 4];
                snprintf(ph, sizeof(ph), "/%s/", w->phonetic);
                cjk_text_draw(UI_MARGIN_X, UI_PHON_TOP, UI_AUX_LEVEL,
                              ph, EPD_GFX_BLACK);
            }
        }
    }

    /* 收藏标记（P1）：已收藏词在音标行右缘显示 *（SET 长按切换；
     * 点阵 ASCII 与音标行同 16px 级，2026-08-23 随音标行点阵化统一）；
     * 听写遮蔽态照画（无拼写信息量）。墨封角标（2026-09-04）：
     * 空心方印「熟」在 * 左侧，右缘锚点依次左移拼排 */
    {
        int wi = study_mode_current_word_index();
        int rx = epd_gfx_width() - UI_MARGIN_X;
        if (learning_state_is_collected(wi)) {
            int sw = cjk_text_width(UI_AUX_LEVEL, "*");
            rx -= sw;
            cjk_text_draw(rx, UI_PHON_TOP, UI_AUX_LEVEL, "*", EPD_GFX_BLACK);
        }
        if (learning_state_is_mastered(wi))
            ui_draw_seal_mark(rx - 4, UI_PHON_TOP);
    }

    /* 正文：cjk 点阵混排（中文按字断/ASCII 按词断，超宽自动换行），
     * 行数按屏高派生，超出一屏分页（多页时首行右缘页码指示）；
     * 遮蔽态见 ui_draw_hidden_body */
    if (study_mode_is_revealed())
        ui_draw_mean_paged(w);
    else
        ui_draw_hidden_body();

    ui_draw_foot(w, UI_BODY_MAX_W);
}

/* LAN 直传外部内容整帧直刷后调用：GFX previous 缓冲已失配，
 * 置 s_last_mode 无效值强制下一次学习界面渲染走全刷 */
void ui_force_full_refresh_next(void)
{
    s_last_mode = MODE_COUNT;
}

/* v1.2 T2.5：设置字号档变更后的排版失效（settings_ui 退出时调用）——
 * UI_MEAN_LEVEL 派生几何变化须全刷重排，释义分页游标与绑定词一并归零 */
void ui_force_font_refresh(void)
{
    s_last_mode = MODE_COUNT;
    s_mean_page = 0;
    s_mean_word = -1;
}

/* 旋转意图 → 绝对旋转映射（2026-08-26 屏幕方向设置）：意图相对面板
 * 默认方向表达（settings_rotation_mode：0=跟随面板/1=竖屏/2=横屏），
 * ^1 翻转奇偶且保持 180° 相位与面板默认一致；不存绝对值——同一 NVS
 * 键跨面板（重编译换屏）语义不漂移 */
static uint8_t rotation_for_intent(int intent)
{
    uint8_t def = epd_panel_default_rotation();
    if (intent == 1) return (def & 1) ? (uint8_t)(def ^ 1) : def;  /* 竖屏 */
    if (intent == 2) return (def & 1) ? def : (uint8_t)(def ^ 1);  /* 横屏 */
    return def;
}

/* 屏幕方向生效链（settings_ui case 6 即改即调；setup 启动恢复亦调，
 * 幂等——映射值与当前一致时零动作）：
 *   1. epd_set_rotation 重建双层画布（帧缓冲面板物理帧与旋转无关）；
 *   2. ui_force_font_refresh 同款布局失效（强制全刷 + 释义分页归零）；
 *   3. 待机页差分影子/引文态失效（下一次渲染走全刷）；
 *   4. READER 页表按当前页 anchor 重建（字号步进 0 = 仅重建页表，
 *      R_MAX_W/H 随新几何；设置页激活时内部 ui_render_word 被拦截，
 *      游标已更新、退出设置页后全刷恢复）。
 * 本函数不绘制——调用方负责（设置页自身 draw_page(true) 全刷重排 /
 * 首帧流程自然渲染；layout_profile 短边分档，横竖切换短边不变档位
 * 稳定，无需失效） */
void ui_apply_rotation(void)
{
    uint8_t rot = rotation_for_intent(settings_rotation_mode());
    if (epd_get_rotation() == rot) return;
    if (epd_set_rotation(rot) != 0) return;
    ui_force_font_refresh();
    standby_invalidate_layout();
    if (study_mode_current() == MODE_READER)
        study_mode_reader_font_step(0);
}

/* 单词卡片渲染入口：状态机每次画面变化时调用 */
void ui_render_word(study_mode_t mode, int index)
{
    if (page_router_top_owns_display()) return; /* 自绘页/LAN 独占期间
                                                * 不绘制（quiz/chat 栈顶放行，缺陷修复） */
    if (study_mode_pron_active() ||
        study_mode_pron_ui_visible()) return; /* P1 跟读三态屏独占内容区 */

    /* 模式切换重置复习列表态（详情态只在会话内保持） */
    if (mode != s_last_mode) review_ui_reset_detail();

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

    /* 复习模式列表态（2026-08-24）：到期词表 + 中键详情；空序列显
     * 示占位空态页（低频，一律整屏全刷——空态下无按键触发重绘） */
    if (mode == MODE_REVIEW && !review_ui_is_detail()) {
        if (study_mode_seq_total() == 0) {
            epd_gfx_fill_screen(EPD_GFX_WHITE);
            ui_draw_status(mode);          /* 序号 0/0 */
            /* T2.4 空态升级：新词达标且无到期词 → 任务完成问候页
             * （否则维持原引导文案；✓ 不在三级字库收录集，纯文字 */
            bool done = daily_plan_done();
            const char *t = done ? "今日任务完成" : "今日无到期词";
            int wpx = cjk_text_width(UI_MEAN_LEVEL, t);
            cjk_text_draw((epd_gfx_width() - wpx) / 2,
                          (epd_gfx_height() - UI_STATUS_H) / 2 + UI_STATUS_H - 20,
                          UI_MEAN_LEVEL, t, EPD_GFX_BLACK);
            const char *h = done ? "明天再来复习" : "新词学习请进闪卡模式";
            wpx = cjk_text_width(0, h);
            cjk_text_draw((epd_gfx_width() - wpx) / 2,
                          (epd_gfx_height() - UI_STATUS_H) / 2 + UI_STATUS_H + 8,
                          0, h, EPD_GFX_BLACK);
            epd_gfx_flush();
            s_last_mode = mode;
            return;
        }

        bool need_full = (mode != s_last_mode);
        if (!need_full && refresh_gfx_before_partial()) need_full = true;

        if (need_full) ui_draw_status(mode);
        review_ui_render_list();

        if (need_full)
            epd_gfx_flush();               /* 整屏全刷 */
        else
            epd_gfx_flush_window(0, UI_STATUS_H,
                                 epd_gfx_width(), epd_gfx_height() - UI_STATUS_H);
        s_last_mode = mode;
        return;
    }

    /* 快速测验视图（v1.2 T2.2）：临时视图自绘状态栏（题号 i+1/N）+
     * 题干 + 选项；换题/反馈局刷含状态栏（题号随题变化，不同于
     * 词卡页仅内容区局刷）；小结页经 s_last_mode=MODE_COUNT 强制
     * 全刷（低频帧红线）；词库空判断不适用（enter 前置词库 ≥8） */
    if (mode == MODE_QUIZ) {
        bool need_full = (mode != s_last_mode);
        if (!need_full && refresh_gfx_before_partial()) need_full = true;

        quiz_ui_render();               /* 测验绘制（quiz_ui.c） */
        if (need_full)
            epd_gfx_flush();            /* 整屏全刷 */
        else
            epd_gfx_flush_window(0, 0,
                                 epd_gfx_width(), epd_gfx_height());
        s_last_mode = mode;
        return;
    }

    /* 墨封全过滤空态（2026-09-04）：闪卡/听写 active 视图清空但词库
     * 非空 → 专用空态页（低频整屏全刷，review 空态同款先例——空态
     * 下无按键触发重绘）；词库空不达此（base_render 分流待机页） */
    if ((mode == MODE_FLASH || mode == MODE_DICTATION) &&
        word_parser_get_count() > 0 && study_mode_seq_total() == 0) {
        epd_gfx_fill_screen(EPD_GFX_WHITE);
        ui_draw_status(mode);          /* 序号 0/0 */
        const char *t = "全部词已墨封";
        int wpx = cjk_text_width(UI_MEAN_LEVEL, t);
        cjk_text_draw((epd_gfx_width() - wpx) / 2,
                      (epd_gfx_height() - UI_STATUS_H) / 2 + UI_STATUS_H - 20,
                      UI_MEAN_LEVEL, t, EPD_GFX_BLACK);
        const char *h = "菜单·墨封录·可启封";   /* 间隔号在字库全角标点集内 */
        wpx = cjk_text_width(0, h);
        cjk_text_draw((epd_gfx_width() - wpx) / 2,
                      (epd_gfx_height() - UI_STATUS_H) / 2 + UI_STATUS_H + 8,
                      0, h, EPD_GFX_BLACK);
        epd_gfx_flush();
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

/* P1 跟读评测三态屏显（pron_task 驱动；状态栏不动，内容区局刷 350ms
 * 级，符合对话环路禁全刷红线。反哺 P2B：chat_mode 状态区同策略）。
 * FAIL 态 total 复用透传错误码：-3=未听到话音，其余=网络/录音失败 */
void ui_render_pron(pron_state_t st, int total, const char *engine)
{
    if (page_router_top_owns_display())
        return;                              /* 自绘页/LAN 独占期间不绘制 */

    epd_gfx_fill_rect(0, UI_STATUS_H, epd_gfx_width(),
                      epd_gfx_height() - UI_STATUS_H, EPD_GFX_WHITE);

    char buf[32];
    switch (st) {
    case PRON_STATE_RECORDING:
        epd_gfx_draw_text(UI_MARGIN_X, UI_WORD_BASE, "Speak now",
                          EPD_GFX_BLACK,
                          ui_fit_font("Speak now", 4, UI_BODY_MAX_W));
        cjk_text_draw(UI_MARGIN_X, UI_BODY_TOP, UI_MEAN_LEVEL,
                      "请跟读 · 按任意键取消", EPD_GFX_BLACK);
        break;
    case PRON_STATE_SCORING:
        epd_gfx_draw_text(UI_MARGIN_X, UI_WORD_BASE, "Scoring...",
                          EPD_GFX_BLACK, 2);
        cjk_text_draw(UI_MARGIN_X, UI_BODY_TOP, UI_MEAN_LEVEL,
                      "评分中", EPD_GFX_BLACK);
        break;
    case PRON_STATE_RESULT:
        /* 大分数 + 通过判定（≥60，与 haptic 映射同阈值）+ 引擎角标
         * （heuristic=基础评分 / gop=精细评分，SPEECH 文档约定） */
        snprintf(buf, sizeof(buf), "%d", total);
        epd_gfx_draw_text(UI_MARGIN_X, UI_WORD_BASE, buf, EPD_GFX_BLACK, 4);
        epd_gfx_draw_text(UI_MARGIN_X + 90, UI_WORD_BASE,
                          total >= 60 ? "Pass!" : "Try again",
                          EPD_GFX_BLACK, 2);
        cjk_text_draw(UI_MARGIN_X, UI_BODY_TOP, UI_MEAN_LEVEL,
                      (engine && strcmp(engine, "gop") == 0)
                          ? "精细评分 · 任意键返回" : "基础评分 · 任意键返回",
                      EPD_GFX_BLACK);
        break;
    case PRON_STATE_FAIL:
        epd_gfx_draw_text(UI_MARGIN_X, UI_WORD_BASE,
                          total == -3 ? "No audio" : "Failed",
                          EPD_GFX_BLACK, 2);
        cjk_text_draw(UI_MARGIN_X, UI_BODY_TOP, UI_MEAN_LEVEL,
                      total == -3 ? "未听到跟读 · 请靠近再试"
                                  : "评分失败 · 稍后再试",
                      EPD_GFX_BLACK);
        break;
    }

    epd_gfx_flush_window(0, UI_STATUS_H, epd_gfx_width(),
                         epd_gfx_height() - UI_STATUS_H);
    LOG_I("pron ui state=%d total=%d", (int)st, total);
}
