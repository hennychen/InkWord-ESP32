/**
 * @file menu_ui.c
 * @brief 快捷菜单 UI 实现（设计见 docs/MENU_DESIGN.md）
 *
 * 三段式列表页（标题栏/反选高亮列表/提示栏 TINY 省略），列表范式
 * 复用 wifi_config_ui draw_list_page(partial)（2026-08 真机验证）；
 * CJK 标签 16px 点阵（FreeSans 无汉字），右侧徽标混排（ASCII 走
 * FreeSans 基线坐标 / 中文走点阵顶左坐标，两套语义经 draw_badge 统一）。
 *
 * 几何全运行期派生（MU_* 宏 + layout_profile 档位，零特判）：
 *   MID 416x240 项高 44 可见 4 / SMALL 264x176 项高 36 可见 3 /
 *   TINY 122x250|128x296 项高 28 可见 8|9、提示栏省略、CJK 徽标省略、
 *   INFO 页裁至 4 行短值项（IP/PSRAM 长值 122px 宽放不下，bring-up 再调）。
 *   按键说明页同理：值列宽 TINY 档不足，bring-up 后改单列两行/键。
 *
 * 字号按档位派生（2026-08-23 真机反馈 16px 偏小）：主内容（列表/标题/
 * 模式页/INFO/按键说明）MID/SMALL 用 20px 点阵、TINY 16px；ASCII 徽标
 * FreeSans size 2|1；提示栏维持 16px 辅助小字。
 *
 * 刷新策略（抄 wifi_config_ui 惯例）：进入/换页全刷；光标移动清列表区
 * 重绘 + 单遍局刷（~350ms）；局刷计数达 MENU_UI_PARTIAL_MAX(10) 经
 * refresh_scheduler 升级全刷保养；三色屏 partial_enabled=false 由
 * epd_gfx_flush_window_passes 内部自动降级全刷，无需特判。
 */
#include "menu_ui.h"
#include "debug_log.h"
#include "epd_driver.h"
#include "cjk_text.h"
#include "layout_profile.h"
#include "refresh_scheduler.h"
#include "haptic.h"
#include "wifi_manager.h"
#include "wifi_config_ui.h"
#include "lan_display_server.h"
#include "audio_sync.h"     /* P0C：音频同步徽标/后台任务启动 */
#include "learning_state.h"
#include "study_mode_machine.h"
#include "word_parser.h"

#include "esp_timer.h"
#include "esp_heap_caps.h"   /* INFO 页 PSRAM 查询 */

#include <string.h>
#include <stdio.h>

static const char *TAG = "MENU_UI";

/* main.cpp 导出（study_mode_machine.c 引用 ui_render_word 同款先例） */
extern void ui_render_current(void);
extern const char *fw_version(void);

/* ---- 几何派生（MENU_DESIGN §4.2，全档运行期） ---- */
#define MU_TINY     (layout_profile_get()->kind == LAYOUT_TINY)
#define MU_SMALL    (layout_profile_get()->kind == LAYOUT_SMALL)
#define MU_TITLE_H  (MU_TINY ? 24 : 32)                    /* 对齐 UI_STATUS_H */
#define MU_ITEM_H   (MU_TINY ? 28 : MU_SMALL ? 36 : 44)
#define MU_FONT_H   (MU_TINY ? 16 : 20)                    /* 主内容字号（px） */
#define MU_FONT_LVL (MU_TINY ? 0  : 1)                     /* cjk_text level */
#define MU_FONT_ASC (MU_TINY ? 1  : 2)                     /* ASCII 徽标 FreeSans size */
#define MU_HINT_H   (MU_TINY ? 0 : 22)                     /* TINY 省略提示栏 */
#define MU_LIST_TOP (MU_TITLE_H + 2)
#define MU_LIST_H   (epd_gfx_height() - MU_TITLE_H - MU_HINT_H)
#define MU_VISIBLE  (MU_LIST_H / MU_ITEM_H)   /* MID=4/SMALL=3/TINY 122x250=8、128x296=9 */
#define MU_MARGIN_X (MU_TINY ? 8 : 16)
#define MU_ITEM_W   (epd_gfx_width() - 2 * MU_MARGIN_X)
#define MU_SB_W     4    /* 滚动条宽 */
#define MU_LABEL_W  88   /* INFO 页标签列宽（「收藏/错词」=72px 余量） */
#define MU_KEYS_LBL_W 56 /* 按键说明页键名列宽（20px 档「上/下」=50px） */
#define MU_INFO_LH  (MU_TINY ? 20 : 28)      /* INFO/按键说明行高（随字号） */
#define MU_INFO_ROWS (MU_TINY ? 4 : 6)   /* TINY 裁长值项（IP/PSRAM） */

/* 刷新策略 */
#define MENU_UI_PARTIAL_MAX  10  /* 局刷阈值（对齐 WIFI_UI_PARTIAL_MAX） */

/* ---- 模块状态（静态零初始化，无 init 无堆分配；~10B） ---- */
typedef enum { MU_PAGE_MAIN = 0, MU_PAGE_MODE, MU_PAGE_INFO, MU_PAGE_KEYS } mu_page_t;

typedef struct {
    const char *label;                    /* UTF-8 CJK 标签 */
    void (*badge)(char *buf, size_t n);   /* 可选 NULL：右侧徽标（ASCII 或 UTF-8） */
    void (*activate)(void);               /* 中键确认动作 */
} mu_item_t;

static bool      s_active = false;
static mu_page_t s_page   = MU_PAGE_MAIN;
static int       s_sel    = 0;   /* 主列表选中（0 基） */
static int       s_off    = 0;   /* 主列表滚动偏移 */
static int       s_mode_sel = 0; /* 模式列表选中（进入时预定位当前模式） */
static int       s_keys_page = 0; /* 按键说明页页码 */

/* 可选模式中文名（二级列表与主菜单徽标共用） */
static const char *s_mode_labels[4] = { "闪卡", "听写", "复习", "阅读" };

static const char *mu_mode_label(study_mode_t m)
{
    if (m == MODE_WRONGBOOK || m == MODE_COLLECTION) m = MODE_FLASH;
    return s_mode_labels[m];
}

/* 前向声明（activate 引用 exit/二级页绘制；绘制区引用菜单项表） */
static void menu_ui_exit(void);
static void draw_main(bool partial);
static void draw_mode(bool partial);
static void draw_info(void);
static void draw_keys(bool partial);

/* ============================================================
 * 徽标填充（每次重绘现取：均为廉价查询，无缓存失效问题）
 * ============================================================ */

static void badge_collected(char *buf, size_t n)
{
    int c = learning_state_collected_count();
    snprintf(buf, n, c == 0 ? "空" : "%d", c);
}

static void badge_mode(char *buf, size_t n)
{
    snprintf(buf, n, "%s", mu_mode_label(study_mode_current()));
}

static void badge_wifi(char *buf, size_t n)
{
    snprintf(buf, n, "%s", wifi_is_connected() ? "已连接" : "未连接");
}

/* 音频同步徽标：同步中「...」/未统计「?」/闲时「缺N/云总M」（纯 ASCII，
 * TINY 档可显）；缺失数读 audio_sync 缓存，任务结束时自动更新 */
static void badge_audio_sync(char *buf, size_t n)
{
    if (audio_sync_is_running()) {
        snprintf(buf, n, "...");
        return;
    }
    int miss = audio_sync_missing_cached();
    if (miss < 0)
        snprintf(buf, n, "?");
    else
        snprintf(buf, n, "%d/%d", miss, audio_sync_cloud_total());
}

/* ============================================================
 * activate 动作（「先 exit 后 enter」纪律：启动子功能前菜单自我
 * 退出（不恢复渲染——子功能自我管理屏幕，避免学习页闪现浪费一次
 * 全刷）；恢复型退出（SET/RST/模式确认）走 menu_ui_exit_restore）
 * ============================================================ */

static void act_collection(void)
{
    if (learning_state_collected_count() == 0) {
        haptic_event(HAPTIC_ERROR);   /* 空收藏：长震边界反馈不进入 */
        return;
    }
    menu_ui_exit();
    study_mode_enter_collection();   /* 计数已预检非零，必成功 */
    ui_render_current();
}

static void act_modesel(void)
{
    s_page = MU_PAGE_MODE;
    s_mode_sel = study_mode_current();   /* 光标预定位当前模式（=「当前」标记） */
    if (s_mode_sel > 3) s_mode_sel = 0;  /* 临时视图（WRONGBOOK/COLLECTION）回闪卡 */
    draw_mode(false);
}

static void act_wifi(void)
{
    menu_ui_exit();
    wifi_config_ui_enter();   /* 异步入队自我管理屏幕 */
}

static void act_portal(void)
{
    menu_ui_exit();
    lan_portal_enter();
}

static void act_lan(void)
{
    menu_ui_exit();
    lan_server_enter_receive_page();
}

/* AI 对话（P2B）：前置预检在 study_mode_enter_chat 内（Wi-Fi/Key/SD），
 * 不满足长震回学习页；满足则进入对话临时视图（首帧全刷由
 * ui_render_current 的 MODE_CHAT 分流承担） */
static void act_chat(void)
{
    menu_ui_exit();
    if (!study_mode_enter_chat()) {
        haptic_event(HAPTIC_ERROR);   /* 无网/未配 Key/无 SD：边界反馈 */
        ui_render_current();
        return;
    }
    haptic_event(HAPTIC_MODE);        /* 进入新模式 50ms（先例） */
    ui_render_current();
}

/* 音频同步：菜单内唯一非独占后台动作（不 exit 菜单，任务 6KB 栈串行
 * 下载，徽标转「...」，完成双短震反馈）。确认时现算缺失数（阻塞
 * ~1s@5000 词，墨水屏节奏可接受），全齐则不启动 */
static void act_audio_sync(void)
{
    if (audio_sync_is_running()) {   /* 已在跑：边界拒绝 */
        haptic_event(HAPTIC_ERROR);
        return;
    }
    int miss = audio_sync_refresh_stats();
    if (miss < 0) {                  /* 无 SD 卡 */
        haptic_event(HAPTIC_ERROR);
        return;
    }
    if (miss == 0) {                 /* 全齐：轻反馈 + 徽标回 0/N */
        haptic_event(HAPTIC_KEYPRESS);
        draw_main(true);
        return;
    }
    if (audio_sync_start() != 0) {   /* Wi-Fi 断/未配 Key */
        haptic_event(HAPTIC_ERROR);
        return;
    }
    haptic_event(HAPTIC_MODE);
    draw_main(true);                 /* 徽标转「...」，任务后台跑 */
}

static void act_info(void)
{
    s_page = MU_PAGE_INFO;
    draw_info();
}

static void act_keys(void)
{
    s_page = MU_PAGE_KEYS;
    s_keys_page = 0;
    draw_keys(false);
}

/* 一期 9 项（二期设置/词书：数组追加即扩展点） */
static const mu_item_t s_items[] = {
    { "收藏列表",   badge_collected,  act_collection },
    { "模式选择",   badge_mode,       act_modesel },
    { "AI 对话",    NULL,             act_chat },
    { "音频同步",   badge_audio_sync, act_audio_sync },
    { "Wi-Fi 配网", badge_wifi,       act_wifi },
    { "AP 配网门户", NULL,            act_portal },
    { "LAN 接收页", NULL,            act_lan },
    { "设备信息",   NULL,          act_info },
    { "按键说明",   NULL,          act_keys },
};
#define MU_ITEM_COUNT ((int)(sizeof(s_items) / sizeof(s_items[0])))

/* ============================================================
 * 绘制
 * ============================================================ */

/* 徽标右对齐绘制：ASCII 走 FreeSans（基线 y = 行内 3/4），中文走点阵
 * （字号随档位，顶左 y 垂直居中）；TINY 档宽度不足（106/112px）仅放 ASCII 徽标 */
static void draw_badge(int right_x, int item_y, const char *text, uint16_t color)
{
    bool wide = cjk_text_has_wide(text);
    if (wide && MU_TINY) return;

    if (wide) {
        int w = cjk_text_width(MU_FONT_LVL, text);
        cjk_text_draw(right_x - w, item_y + (MU_ITEM_H - MU_FONT_H) / 2,
                      MU_FONT_LVL, text, color);
    } else {
        int tw, th;
        epd_gfx_text_bounds(text, MU_FONT_ASC, &tw, &th);
        epd_gfx_draw_text(right_x - tw, item_y + MU_ITEM_H * 3 / 4,
                          text, color, MU_FONT_ASC);
    }
}

/* 单项绘制：反选高亮（黑底白字）+ 左 CJK 标签 + 右徽标 */
static void draw_item(int idx, int row, const mu_item_t *item)
{
    int y = MU_LIST_TOP + row * MU_ITEM_H;
    int w = MU_ITEM_W - MU_SB_W - 4;   /* 列表主体宽（右侧留滚动条） */
    bool sel = (idx == s_sel);

    if (sel)
        epd_gfx_fill_rect(MU_MARGIN_X, y, w, MU_ITEM_H - 4, EPD_GFX_BLACK);

    cjk_text_draw(MU_MARGIN_X + 4, y + (MU_ITEM_H - MU_FONT_H) / 2,
                  MU_FONT_LVL, item->label,
                  sel ? EPD_GFX_WHITE : EPD_GFX_BLACK);

    if (item->badge) {
        char buf[16];
        item->badge(buf, sizeof(buf));
        if (buf[0])
            draw_badge(MU_MARGIN_X + w - 6, y, buf,
                       sel ? EPD_GFX_WHITE : EPD_GFX_BLACK);
    }
}

/* 右侧滚动条（项数超出可见区时；抄 wifi_config_ui 滑块范式） */
static void draw_scrollbar(int total)
{
    if (total <= MU_VISIBLE) return;
    int x  = epd_gfx_width() - MU_MARGIN_X;
    int h  = MU_VISIBLE * MU_ITEM_H;
    int y0 = MU_LIST_TOP;
    epd_gfx_draw_rect(x, y0, MU_SB_W, h, EPD_GFX_BLACK);
    int thumb_h = h * MU_VISIBLE / total;
    if (thumb_h < MU_SB_W * 2) thumb_h = MU_SB_W * 2;   /* 最小滑块 */
    int thumb_y = y0 + (h - thumb_h) * s_off / (total - MU_VISIBLE);
    epd_gfx_fill_rect(x, thumb_y, MU_SB_W, thumb_h, EPD_GFX_BLACK);
}

/* 标题栏：白底黑字（同学习页状态栏风格）+ 右侧序号 + 分隔线 */
static void draw_title(const char *title, int sel_1based, int total)
{
    epd_gfx_fill_rect(0, 0, epd_gfx_width(), MU_TITLE_H, EPD_GFX_WHITE);
    cjk_text_draw(MU_MARGIN_X, (MU_TITLE_H - MU_FONT_H) / 2,
                  MU_FONT_LVL, title, EPD_GFX_BLACK);
    if (total > 0) {
        char buf[24];   /* "%d/%d" 极值 11+1+11=23，避免 -Wformat-truncation */
        snprintf(buf, sizeof(buf), "%d/%d", sel_1based, total);
        int tw, th;
        epd_gfx_text_bounds(buf, MU_FONT_ASC, &tw, &th);
        epd_gfx_draw_text(epd_gfx_width() - MU_MARGIN_X - tw,
                          MU_TITLE_H - 10, buf, EPD_GFX_BLACK, MU_FONT_ASC);
    }
    epd_gfx_draw_hline(MU_MARGIN_X, MU_TITLE_H,
                       epd_gfx_width() - 2 * MU_MARGIN_X, EPD_GFX_BLACK);
}

/* 底部提示栏（TINY 档省略；cjk_text 混排原生支持 ASCII 片段） */
static void draw_hint(void)
{
    if (MU_HINT_H == 0) return;
    epd_gfx_draw_hline(MU_MARGIN_X, epd_gfx_height() - MU_HINT_H,
                       epd_gfx_width() - 2 * MU_MARGIN_X, EPD_GFX_BLACK);
    cjk_text_draw(MU_MARGIN_X,
                  epd_gfx_height() - MU_HINT_H + (MU_HINT_H - 16) / 2,
                  0, "上/下 选择  中 确认  SET 返回", EPD_GFX_BLACK);
}

static void draw_flush(void)
{
    epd_power_on();
    epd_gfx_flush();
    epd_power_off();
}

/* 局刷路径：清标题栏以下重绘列表体 + 单遍局刷（阈值预检在调用方） */
static void partial_refresh(void (*body)(void))
{
    epd_gfx_fill_rect(0, MU_TITLE_H, epd_gfx_width(),
                      epd_gfx_height() - MU_TITLE_H, EPD_GFX_WHITE);
    body();
    epd_gfx_flush_window_passes(0, MU_TITLE_H, epd_gfx_width(),
                                epd_gfx_height() - MU_TITLE_H, 1);
}

/* ---- 主菜单页 ---- */

static void draw_main_body(void)
{
    for (int i = 0; i < MU_VISIBLE; i++) {
        int idx = s_off + i;
        if (idx >= MU_ITEM_COUNT) break;
        draw_item(idx, i, &s_items[idx]);
    }
    draw_scrollbar(MU_ITEM_COUNT);
}

/* ---- 二级模式列表页（4 项，光标预定位当前模式即「当前」标记，
 *      与 ● 标记信息等价且零字库依赖） ---- */

static void draw_mode_body(void)
{
    for (int i = 0; i < 4 && i < MU_VISIBLE; i++) {
        int y = MU_LIST_TOP + i * MU_ITEM_H;
        bool sel = (i == s_mode_sel);
        if (sel)
            epd_gfx_fill_rect(MU_MARGIN_X, y, MU_ITEM_W, MU_ITEM_H - 4,
                              EPD_GFX_BLACK);
        cjk_text_draw(MU_MARGIN_X + 4, y + (MU_ITEM_H - MU_FONT_H) / 2,
                      MU_FONT_LVL,
                      s_mode_labels[i], sel ? EPD_GFX_WHITE : EPD_GFX_BLACK);
    }
}

/* ---- 设备信息页（静态只读，进入时一次性取值，全刷） ---- */

static void draw_info_row(int row, const char *label, const char *value)
{
    int y = MU_LIST_TOP + 4 + row * MU_INFO_LH;
    cjk_text_draw(MU_MARGIN_X, y, MU_FONT_LVL, label, EPD_GFX_BLACK);
    cjk_text_draw(MU_MARGIN_X + MU_LABEL_W, y, MU_FONT_LVL, value,
                  EPD_GFX_BLACK);
}

static void draw_info(void)
{
    /* 数组顺序 = 显示顺序；TINY 只绘前 MU_INFO_ROWS 行（短值项），
     * IP/PSRAM 长值 122px 宽放不下（bring-up 后再调） */
    static const char *labels[6] = {
        "固件版本", "词库", "收藏/错词", "运行时长", "IP 地址", "PSRAM"
    };
    char v[6][40];

    snprintf(v[0], sizeof(v[0]), "%s", fw_version());
    snprintf(v[1], sizeof(v[1]), "%d 词", word_parser_get_count());
    snprintf(v[2], sizeof(v[2]), "%d / %d",
             learning_state_collected_count(), learning_state_wrong_count());
    int64_t up = esp_timer_get_time() / 1000000LL;
    snprintf(v[3], sizeof(v[3]), "%02lld:%02lld:%02lld",
             (long long)(up / 3600), (long long)(up / 60 % 60),
             (long long)(up % 60));
    if (!wifi_get_sta_ip(v[4], sizeof(v[4])))
        snprintf(v[4], sizeof(v[4]), "--");
    snprintf(v[5], sizeof(v[5]), "%.1f/%.1f MB",
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1048576.0,
             heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1048576.0);

    epd_gfx_fill_screen(EPD_GFX_WHITE);
    draw_title("设备信息", 0, 0);
    for (int i = 0; i < MU_INFO_ROWS; i++)
        draw_info_row(i, labels[i], v[i]);
    draw_hint();
    draw_flush();
}

/* ---- 按键说明页（表驱动分页只读，上/下翻页；行文全半角标点=字库安全
 *      先例，组头行 key=NULL 整行居左作小节分隔） ---- */

typedef struct {
    const char *key;    /* 键名（NULL = 组头行） */
    const char *text;   /* 「短按 / 长按」两段式 */
} mu_keyrow_t;

static const mu_keyrow_t s_keys[] = {
    { NULL,     "[ 学习页 ]" },
    { "上",     "上一词 / 清残影" },
    { "下",     "下一词 / 换模式" },
    { "左",     "自评忘记 / AP 门户" },
    { "右",     "自评简单 / LAN 页" },
    { "中",     "发音 / 功能菜单" },
    { "SET",    "遮蔽 / 收藏切换" },
    { "RST",    "回本组首 / 错词本" },
    { "*",      "词卡已收藏标记" },
    { NULL,     "[ 收藏/错词视图 ]" },
    { "上/下",  "序列内翻词" },
    { "中",     "发音" },
    { "SET",    "遮蔽 / 取消收藏" },
    { "RST",    "回首词 / 退出视图" },
    { NULL,     "[ AI 对话 ]" },
    { "中",     "说话·发送·重说" },
    { "RST",    "退出回闪卡" },
    { NULL,     "[ 待机页 ]" },
    { "中",     "拉天气 / 功能菜单" },
    { "SET",    "轮换引文" },
    { NULL,     "[ 菜单内 ]" },
    { "上/下",  "移动选择" },
    { "中",     "确认 / 进入" },
    { "SET",    "返回 / 主层退出" },
    { "RST",    "退出回原页面" },
};
#define MU_KEYS_COUNT ((int)(sizeof(s_keys) / sizeof(s_keys[0])))
#define MU_KEYS_ROWS  (MU_LIST_H / MU_INFO_LH)   /* MID 6 / TINY 11 */

static int keys_page_count(void)
{
    return (MU_KEYS_COUNT + MU_KEYS_ROWS - 1) / MU_KEYS_ROWS;
}

static void draw_keys_body(void)
{
    int start = s_keys_page * MU_KEYS_ROWS;
    for (int i = 0; i < MU_KEYS_ROWS; i++) {
        int idx = start + i;
        if (idx >= MU_KEYS_COUNT) break;
        int y = MU_LIST_TOP + 2 + i * MU_INFO_LH;
        if (s_keys[idx].key) {
            cjk_text_draw(MU_MARGIN_X + MU_KEYS_LBL_W, y, MU_FONT_LVL,
                          s_keys[idx].text, EPD_GFX_BLACK);
            cjk_text_draw(MU_MARGIN_X, y, MU_FONT_LVL,
                          s_keys[idx].key, EPD_GFX_BLACK);
        } else {
            cjk_text_draw(MU_MARGIN_X, y, MU_FONT_LVL,
                          s_keys[idx].text, EPD_GFX_BLACK);
        }
    }
}

static void draw_keys(bool partial)
{
    if (partial && !refresh_gfx_before_partial_n(MENU_UI_PARTIAL_MAX)) {
        partial_refresh(draw_keys_body);
        return;
    }
    epd_gfx_fill_screen(EPD_GFX_WHITE);
    draw_title("按键说明", s_keys_page + 1, keys_page_count());
    draw_keys_body();
    draw_hint();
    draw_flush();
}

/* ---- 页面绘制入口（partial=true 局刷路径 + 阈值升级全刷） ---- */

static void draw_main(bool partial)
{
    if (partial && !refresh_gfx_before_partial_n(MENU_UI_PARTIAL_MAX)) {
        partial_refresh(draw_main_body);
        return;
    }
    epd_gfx_fill_screen(EPD_GFX_WHITE);
    draw_title("功能菜单", s_sel + 1, MU_ITEM_COUNT);
    draw_main_body();
    draw_hint();
    draw_flush();
}

static void draw_mode(bool partial)
{
    if (partial && !refresh_gfx_before_partial_n(MENU_UI_PARTIAL_MAX)) {
        partial_refresh(draw_mode_body);
        return;
    }
    epd_gfx_fill_screen(EPD_GFX_WHITE);
    draw_title("模式选择", s_mode_sel + 1, 4);
    draw_mode_body();
    draw_hint();
    draw_flush();
}

/* ============================================================
 * 按键处理（激活时独占；长按全部忽略防误触）
 * ============================================================ */

static void menu_ui_exit(void)
{
    s_active = false;
    s_page   = MU_PAGE_MAIN;
    s_sel = s_off = 0;
}

/* 恢复型退出（SET 主菜单层 / RST 任意层级）：exit 后经
 * ui_render_current 恢复学习页/待机页（模式变化时自然全刷） */
static void menu_ui_exit_restore(void)
{
    menu_ui_exit();
    ui_render_current();
}

/* 光标移动（循环滚动）+ 滚动窗口跟随 + 局刷重绘 */
static void main_move(int dir)
{
    s_sel = (s_sel + dir + MU_ITEM_COUNT) % MU_ITEM_COUNT;
    if (s_sel < s_off) s_off = s_sel;
    if (s_sel >= s_off + MU_VISIBLE) s_off = s_sel - MU_VISIBLE + 1;
    draw_main(true);
}

void menu_ui_on_button(nav_key_t id, button_event_t event)
{
    if (!s_active) return;
    if (event != BUTTON_EVENT_SHORT_PRESS) return;   /* 长按全部忽略 */

    switch (s_page) {
    case MU_PAGE_MAIN:
        switch (id) {
        case NAV_UP:     main_move(-1); break;
        case NAV_DOWN:   main_move(+1); break;
        case NAV_CENTER: s_items[s_sel].activate(); break;
        case NAV_SET:    menu_ui_exit_restore(); break;   /* 主菜单层=退出 */
        case NAV_RST:    menu_ui_exit_restore(); break;
        default: break;   /* 左/右忽略 */
        }
        break;

    case MU_PAGE_MODE:
        switch (id) {
        case NAV_UP:
            s_mode_sel = (s_mode_sel + 3) % 4;
            draw_mode(true);
            break;
        case NAV_DOWN:
            s_mode_sel = (s_mode_sel + 1) % 4;
            draw_mode(true);
            break;
        case NAV_CENTER:
            /* 与长按下循环终态一致：study_mode_set 全副作用
             * （NVS 持久化/游标归零/READER 进度恢复） */
            haptic_event(HAPTIC_MODE);
            menu_ui_exit();
            study_mode_set((study_mode_t)s_mode_sel);
            ui_render_current();
            break;
        case NAV_SET:    /* 返回上级 */
            s_page = MU_PAGE_MAIN;
            draw_main(false);
            break;
        case NAV_RST:
            menu_ui_exit_restore();
            break;
        default: break;
        }
        break;

    case MU_PAGE_INFO:
        /* 静态只读页：SET/中返回主菜单，RST 退出菜单 */
        switch (id) {
        case NAV_SET:
        case NAV_CENTER:
            s_page = MU_PAGE_MAIN;
            draw_main(false);
            break;
        case NAV_RST:
            menu_ui_exit_restore();
            break;
        default: break;
        }
        break;

    case MU_PAGE_KEYS:
        /* 分页浏览：上/下翻页（循环），中=下一页，SET 返回，RST 退出 */
        switch (id) {
        case NAV_UP:
            s_keys_page = (s_keys_page + keys_page_count() - 1) % keys_page_count();
            draw_keys(true);
            break;
        case NAV_DOWN:
        case NAV_CENTER:
            s_keys_page = (s_keys_page + 1) % keys_page_count();
            draw_keys(true);
            break;
        case NAV_SET:
            s_page = MU_PAGE_MAIN;
            draw_main(false);
            break;
        case NAV_RST:
            menu_ui_exit_restore();
            break;
        default: break;
        }
        break;

    default:
        break;
    }
}

/* ============================================================
 * 公共 API
 * ============================================================ */

void menu_ui_enter(void)
{
    if (s_active) return;   /* 幂等 */
    s_active = true;
    s_page   = MU_PAGE_MAIN;
    s_sel = s_off = 0;
    haptic_event(HAPTIC_MODE);   /* 进入菜单 50ms（对齐模式切换/错词本） */
    draw_main(false);
    LOG_I("menu entered");
}

bool menu_ui_is_active(void)
{
    return s_active;
}
