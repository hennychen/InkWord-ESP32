/**
 * @file menu_ui.c
 * @brief 大屏精简版菜单 UI 实现 —— 三区范式 + 光标行局刷（2026-09-17 UI 重设计）
 *
 * 三区（对齐词卡页范式）：
 *   顶栏 0~120 黑底白字：「功能菜单」+ 当前模式徽标（右）
 *   主区 120~1000 白底：[学习] 组（模式/收藏/墨封/错词本）+
 *        [阅读] 组（电子书/发送图片）+ [系统] 组（设备信息）。
 *        组头行 70px（24px 点阵），菜单项 140px（32px 点阵标签 +
 *        徽标，全点阵含 ASCII 数字——FreeSans 退出本页），焦点条
 *        黑底白字（PAD 收口全宽，反白即焦点）
 *   底栏 1000~1080 白底：键位提示（上下 选择 · 中键 确认 · SET 返回）
 *
 * 刷新分档：进页/恢复 = GC16 全刷；光标移动 = DU 列表区窗口局刷
 * （无滚动仅重绘新旧两行；滚动时整列表区重绘。DU 差异行自动跳过
 * 未变行，gfx 层 K=8 次局刷自动插全屏重置驱白灰染）。
 *
 * 3 组头 + 7 项 = 1190px > 主区 880 → s_off 滚动窗口实装
 * （ensure_visible 贪心推进；组头可作窗口首行，跳变属滚动惯例）。
 *
 * 色彩纪律：EPD_GFX_WHITE/BLACK 宏（本页由暗色底改亮色词典风，
 * 与词卡/设置页统一——2026-09-17 UI 重设计有意变更）。
 */
#include "menu_ui.h"

#include <stdio.h>
#include <string.h>

#include "cjk_font.h"
#include "cjk_text.h"
#include "debug_log.h"
#include "epd_gfx.h"
#include "lan_portal.h"
#include "learning_state.h"
#include "reader_engine.h"
#include "reader_page.h"
#include "settings_ui.h"
#include "study_mode_machine.h"

static const char *TAG = "MENU";

/* ---- 几何（XLARGE 专属常量，对齐词卡三区） ---- */
#define MENU_TOP_H    120   /* 顶栏高（黑底白字） */
#define MENU_BOTTOM_H  80   /* 底栏高（键位提示带） */
#define MENU_PAD       80   /* 左右收口 */
#define MENU_HDR_H     70   /* 组头行高 */
#define MENU_ITEM_H   140   /* 菜单项行高 */
#define MENU_LIST_Y    MENU_TOP_H
#define MENU_LIST_H    (1080 - MENU_TOP_H - MENU_BOTTOM_H)   /* 880 */

/* ---- 模块状态（静态零初始化） ---- */
static bool s_active = false;
static int  s_sel    = 0;   /* 选中项（0 基，跳过组头） */
static int  s_off    = 0;   /* 滚动窗口首行（全局索引） */

/* ---- 菜单项定义（组头行不可选中；原硬编码 is_group_header(0||4)
 *      与项枚举冲突致 MI_INFO 被吞，2026-09 加阅读组时重排修复） ---- */
typedef enum {
    MI_HDR_STUDY = 0, /* [学习] 组头 */
    MI_MODE,          /* 模式切换 */
    MI_COLLECTION,    /* 收藏 */
    MI_MASTERED,      /* 墨封 */
    MI_WRONGBOOK,     /* 错词本 */
    MI_HDR_READ,      /* [阅读] 组头 */
    MI_READER,        /* 电子书（阅读页覆盖层入口） */
    MI_HDR_SYS,       /* [系统] 组头 */
    MI_LAN,           /* 发送图片（LAN 门户覆盖层入口，ADC2/WiFi 会话互斥） */
    MI_INFO,          /* 设备信息（设置页入口） */
    MI_COUNT
} menu_item_t;

static const char *s_group_labels[] = { "[学习]", "[阅读]", "[系统]" };

static bool is_group_header(int idx)
{
    return idx == MI_HDR_STUDY || idx == MI_HDR_READ || idx == MI_HDR_SYS;
}

/* 组头行标签（非组头返回 NULL） */
static const char *header_label(int idx)
{
    switch (idx) {
    case MI_HDR_STUDY: return s_group_labels[0];
    case MI_HDR_READ:  return s_group_labels[1];
    case MI_HDR_SYS:   return s_group_labels[2];
    default:           return NULL;
    }
}

static const char *item_label(int idx)
{
    switch (idx) {
    case MI_MODE:       return "模式切换";
    case MI_COLLECTION: return "收藏";
    case MI_MASTERED:   return "墨封";
    case MI_WRONGBOOK:  return "错词本";
    case MI_READER:     return "电子书";
    case MI_LAN:        return "发送图片";
    case MI_INFO:       return "设备信息";
    default:            return "";
    }
}

/* 徽标填充（右侧动态值；全点阵可显——中文/数字/短 ASCII） */
static void item_badge(int idx, char *buf, size_t n)
{
    switch (idx) {
    case MI_MODE:
        snprintf(buf, n, "%s", study_mode_current() == MODE_FLASH ? "闪卡" : "复习");
        break;
    case MI_COLLECTION:
        snprintf(buf, n, "%d", learning_state_collected_count());
        break;
    case MI_MASTERED:
        snprintf(buf, n, "%d", learning_state_mastered_count());
        break;
    case MI_WRONGBOOK:
        snprintf(buf, n, "%d", learning_state_wrong_count());
        break;
    case MI_READER:
        if (reader_ready() && reader_page_count() > 0)
            snprintf(buf, n, "%d 页", reader_page_count());
        else
            snprintf(buf, n, "无书");
        break;
    case MI_LAN:
        snprintf(buf, n, "WiFi");
        break;
    case MI_INFO:
        snprintf(buf, n, "v1.0");
        break;
    default:
        buf[0] = '\0';
    }
}

/* 前向声明 */
static void menu_ui_exit(void);

/* ---- 绘制辅助 ---- */

/* 顶栏（黑底白字） */
static void draw_title(void)
{
    epd_gfx_fill_rect(0, 0, 1920, MENU_TOP_H, EPD_GFX_BLACK);
    cjk_text_draw(MENU_PAD, (MENU_TOP_H - cjk_glyph_cell_size(2)) / 2,
                  2, "功能菜单", EPD_GFX_WHITE);

    /* 右侧当前模式徽标（与词卡顶栏同位呼应） */
    char mode_buf[16];
    item_badge(MI_MODE, mode_buf, sizeof(mode_buf));
    cjk_text_draw(1920 - MENU_PAD - cjk_text_width(2, mode_buf),
                  (MENU_TOP_H - cjk_glyph_cell_size(2)) / 2,
                  2, mode_buf, EPD_GFX_WHITE);
}

/* 底栏（白底键位提示带，静态文案） */
static void draw_hint(void)
{
    static const char hint[] = "上下 选择 · 中键 确认 · SET 返回";
    cjk_text_draw((1920 - cjk_text_width(1, hint)) / 2,
                  1080 - MENU_BOTTOM_H +
                      (MENU_BOTTOM_H - cjk_glyph_cell_size(1)) / 2,
                  1, hint, EPD_GFX_BLACK);
}

/* 菜单项（idx=全局索引，y=顶坐标，selected=焦点反白条） */
static void draw_item(int idx, int y, bool selected)
{
    epd_gfx_fill_rect(MENU_PAD, y, 1920 - 2 * MENU_PAD, MENU_ITEM_H,
                      selected ? EPD_GFX_BLACK : EPD_GFX_WHITE);
    uint16_t fg = selected ? EPD_GFX_WHITE : EPD_GFX_BLACK;
    int ty = y + (MENU_ITEM_H - cjk_glyph_cell_size(3)) / 2;

    cjk_text_draw(MENU_PAD + 40, ty, 3, item_label(idx), fg);

    char badge[32];
    item_badge(idx, badge, sizeof(badge));
    if (badge[0])
        cjk_text_draw(1920 - MENU_PAD - 40 - cjk_text_width(3, badge),
                      ty, 3, badge, fg);
}

/* 全局 idx 行顶 y（绝对坐标）；不在 s_off 可见窗口内返回 -1。
 * 自 s_off 逐行累高（10 项线性扫描，无谓紧凑） */
static int row_y(int idx)
{
    int y = MENU_LIST_Y;
    for (int i = s_off; i < MI_COUNT; i++) {
        int h = is_group_header(i) ? MENU_HDR_H : MENU_ITEM_H;
        if (i == idx) return (y + h <= MENU_LIST_Y + MENU_LIST_H) ? y : -1;
        y += h;
    }
    return -1;
}

/* 滚动窗口贪心调整：s_sel 不可见时推进 s_off（sel 在窗口上方即
 * wrap 向上 → sel 直接作首行；否则底部越界 → 窗口下移一行。项高
 * 140 << 880 恒可放下，guard 上限防呆即可） */
static void ensure_visible(void)
{
    for (int guard = 0; guard < 2 * MI_COUNT && row_y(s_sel) < 0; guard++) {
        if (s_sel < s_off) s_off = s_sel;
        else s_off++;
    }
}

/* 列表区全量重绘（先清区——滚动后旧残行必须抹掉） */
static void draw_list(void)
{
    epd_gfx_fill_rect(0, MENU_LIST_Y, 1920, MENU_LIST_H, EPD_GFX_WHITE);
    int y = MENU_LIST_Y;
    for (int i = s_off; i < MI_COUNT; i++) {
        int h = is_group_header(i) ? MENU_HDR_H : MENU_ITEM_H;
        if (y + h > MENU_LIST_Y + MENU_LIST_H) break;
        const char *hdr = header_label(i);
        if (hdr)
            cjk_text_draw(MENU_PAD + 40,
                          y + (MENU_HDR_H - cjk_glyph_cell_size(2)) / 2,
                          2, hdr, EPD_GFX_BLACK);
        else
            draw_item(i, y, i == s_sel);
        y += h;
    }
}

/* 全量绘制 + GC16 全刷（进页/render 恢复路径） */
static void draw_main(void)
{
    epd_gfx_fill_screen(EPD_GFX_WHITE);
    draw_title();
    draw_list();
    draw_hint();
    epd_gfx_flush();
}

/* 光标移动重绘：无滚动仅新旧两行；滚动整列表区。列表区窗口 DU
 * 局刷（未变行 DU 差异跳过，双保险取整区窗口） */
static void cursor_moved(int prev_sel)
{
    int prev_off = s_off;
    ensure_visible();
    if (s_off != prev_off) {
        draw_list();
    } else {
        int py = row_y(prev_sel);
        if (py >= 0) draw_item(prev_sel, py, false);
        draw_item(s_sel, row_y(s_sel), true);
    }
    epd_gfx_flush_window(0, MENU_LIST_Y, 1920, MENU_LIST_H);
}

/* ---- 动作（activate） ---- */

static void act_mode(void)
{
    study_mode_switch_next();
    menu_ui_exit();
    page_router_render_top();
}

static void act_collection(void)
{
    if (learning_state_collected_count() == 0) {
        ESP_LOGW(TAG, "空收藏，拒绝进入");
        return;
    }
    study_mode_enter_collection();
    menu_ui_exit();
    page_router_render_top();
}

static void act_mastered(void)
{
    if (learning_state_mastered_count() == 0) {
        ESP_LOGW(TAG, "空墨封，拒绝进入");
        return;
    }
    study_mode_enter_mastered();
    menu_ui_exit();
    page_router_render_top();
}

static void act_wrongbook(void)
{
    if (learning_state_wrong_count() == 0) {
        ESP_LOGW(TAG, "无错词，拒绝进入");
        return;
    }
    study_mode_enter_wrongbook();
    menu_ui_exit();
    page_router_render_top();
}

static void act_info(void)
{
    settings_ui_enter();
}

static void act_reader(void)
{
    /* 阅读页覆盖层入栈（enter 自绘首帧；引擎惰性初始化在页内） */
    page_router_push(&g_reader_page);
}

static void act_lan(void)
{
    /* LAN 门户页入栈（enter 起 WiFi/httpd 会话 + 暂停按键扫描） */
    page_router_push(&g_lan_portal_page);
}

static void activate(int idx)
{
    switch (idx) {
    case MI_MODE:       act_mode(); break;
    case MI_COLLECTION: act_collection(); break;
    case MI_MASTERED:   act_mastered(); break;
    case MI_WRONGBOOK:  act_wrongbook(); break;
    case MI_READER:     act_reader(); break;
    case MI_LAN:        act_lan(); break;
    case MI_INFO:       act_info(); break;
    default: break;
    }
}

/* ---- 退出 ---- */

static void menu_ui_exit(void)
{
    if (!s_active) return;
    s_active = false;
    page_router_pop_if(&g_menu_ui_page);  /* 弹出菜单页，render_top 自动恢复 base */
}

/* ---- 页面协议 ---- */

static void menu_ui_render(void)
{
    ensure_visible();
    draw_main();
}

static bool menu_ui_on_button_internal(nav_key_t id, button_event_t event)
{
    if (event == BUTTON_EVENT_LONG_PRESS) return true;  /* 忽略长按防误触 */
    if (event != BUTTON_EVENT_SHORT_PRESS) return true;

    switch (id) {
    case NAV_UP: {
        /* 上移（跳过组头） */
        int prev = s_sel;
        do {
            s_sel = (s_sel - 1 + MI_COUNT) % MI_COUNT;
        } while (is_group_header(s_sel));
        cursor_moved(prev);
        return true;
    }
    case NAV_DOWN: {
        /* 下移（跳过组头） */
        int prev = s_sel;
        do {
            s_sel = (s_sel + 1) % MI_COUNT;
        } while (is_group_header(s_sel));
        cursor_moved(prev);
        return true;
    }
    case NAV_CENTER:
        activate(s_sel);
        return true;
    case NAV_SET:
    case NAV_RST:
        menu_ui_exit();
        return true;
    default:
        return true;
    }
}

void menu_ui_on_button(nav_key_t id, button_event_t event)
{
    menu_ui_on_button_internal(id, event);
}

void menu_ui_enter(void)
{
    if (s_active) return;
    s_active = true;
    s_sel = MI_MODE;   /* 首个可选项（组头不可选中） */
    s_off = 0;
    page_router_push(&g_menu_ui_page);
}

const page_t g_menu_ui_page = {
    .name = "menu",
    .render = menu_ui_render,
    .on_button = menu_ui_on_button_internal,
    .enter = NULL,
    .exit = NULL,
    .owns_display = false,
};
