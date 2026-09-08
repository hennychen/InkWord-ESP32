/**
 * @file schedule.c
 * @brief 课程表——周计划编排与自动激活实现（v1.6 新增）
 *
 * 薄模块：不新增持久化表——配置 NVS 单 blob（sched_cfg ~421B），
 * 激活状态 NVS 单 blob（sched_last 12B）；跨日判据复用 standby_time_now
 * 自治钟（daily_plan/learning_state 同源）；自动激活调 deck_flow_switch
 * 完整编排链路（词库重载+LR隔离+阅读scope+渲染刷新），零重复代码。
 *
 * 容错：卡组 id 在 manifest 中不存在 → 跳过该槽不崩溃；时钟未同步 →
 * 拒绝激活（epoch_ymd 返回 0）；配置 blob 长度不匹配 → 退化为默认。
 */
#include "schedule.h"
#include "deck_manager.h"
#include "daily_plan.h"
#include "learning_state.h"
#include "debug_log.h"
#include "epd_driver.h"     /* 测试课表渲染：绘图原语 */
#include "cjk_text.h"       /* 测试课表渲染：CJK 点阵文本 */
#include "layout_profile.h" /* PPI 自动层（2026-09-08）：字级/几何派生 */
#include "cjk_font.h"       /* cjk_glyph_cell_size：字库级→cell px */
#include "page_router.h"    /* 页面路由（课程表入栈） */
#include "haptic.h"         /* 按键反馈 */

#include "nvs.h"
#include "settings_keys.h"   /* P2b：NVS 键权威表 */

#include <string.h>
#include <time.h>

static const char *TAG = "SCHED";

/* 自治钟公共导出（standby_page.c / daily_plan.c 同源） */
extern int64_t standby_time_now(void);

/* main.cpp 导出（deck_flow_switch 完整切组编排） */
extern bool deck_flow_switch(int idx);

/* ---- epoch 日历工具（daily_plan.c 同源副本，零状态纯函数） ---- */

static int32_t epoch_ymd(int64_t epoch)
{
    time_t t = (time_t)(epoch + 8 * 3600);   /* UTC+8 */
    struct tm tmv;
    if (!gmtime_r(&t, &tmv)) return 0;
    return (int32_t)((tmv.tm_year + 1900) * 10000 +
                     (tmv.tm_mon + 1) * 100 + tmv.tm_mday);
}

/* epoch -> 星期几（0=周一~6=周日；C 库 tm_wday 0=周日，需换算） */
static int epoch_wday(int64_t epoch)
{
    time_t t = (time_t)(epoch + 8 * 3600);
    struct tm tmv;
    if (!gmtime_r(&t, &tmv)) return -1;
    /* tm_wday: 0=Sun,1=Mon,...,6=Sat → 换算为 0=Mon,...,6=Sun */
    return (tmv.tm_wday + 6) % 7;
}

/* ---- 自动激活状态 ---- */

typedef struct {
    int32_t last_ymd;        /**< 上次自动激活日期（跨日判据） */
    int8_t  last_slot;       /**< 上次激活的槽位索引 0~3（-1=无有效槽位） */
    char    last_deck[8];    /**< 上次激活的卡组 id */
    bool    manual_override; /**< 当日已手动切组（true=今日不再自动干预） */
} sched_state_t;

/* ---- 模块状态（静态零初始化） ---- */
static schedule_cfg_t    s_cfg;       /* 周配置（默认全零=关闭+空槽） */
static sched_state_t     s_state;     /* 激活状态 */
static schedule_display_t s_disp;     /* 显示课表（LAN Web 编辑器写入） */

/* ---- NVS 读写 ---- */

static void cfg_load(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    schedule_cfg_t tmp;
    size_t len = sizeof(tmp);
    if (nvs_get_blob(h, NVS_KEY_SCHED_CFG, &tmp, &len) == ESP_OK &&
        len == sizeof(tmp)) {
        s_cfg = tmp;
        LOG_I("schedule loaded: enabled=%d", (int)s_cfg.enabled);
    }
    nvs_close(h);
}

static void state_load(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.last_slot = -1;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    sched_state_t tmp;
    size_t len = sizeof(tmp);
    if (nvs_get_blob(h, NVS_KEY_SCHED_STATE, &tmp, &len) == ESP_OK &&
        len == sizeof(tmp)) {
        s_state = tmp;
    }
    nvs_close(h);
}

static void disp_load(void)
{
    memset(&s_disp, 0, sizeof(s_disp));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    schedule_display_t tmp;
    size_t len = sizeof(tmp);
    if (nvs_get_blob(h, NVS_KEY_SCHED_DISP, &tmp, &len) == ESP_OK &&
        len == sizeof(tmp)) {
        s_disp = tmp;
        LOG_I("display table loaded: enabled=%d rows=%d cols=%d",
              (int)s_disp.enabled, s_disp.rows, s_disp.cols);
    }
    nvs_close(h);
}

void schedule_init(void)
{
    cfg_load();
    state_load();
    disp_load();
    LOG_I("schedule init done");
}

const schedule_cfg_t *schedule_cfg(void)
{
    return &s_cfg;
}

schedule_cfg_t *schedule_cfg_mut(void)
{
    return &s_cfg;
}

void schedule_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY_SCHED_CFG, &s_cfg, sizeof(s_cfg));
    nvs_commit(h);
    nvs_close(h);
    LOG_I("schedule config saved");
}

static void state_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY_SCHED_STATE, &s_state, sizeof(s_state));
    nvs_commit(h);
    nvs_close(h);
}

/* ---- 查询 ---- */

bool schedule_is_enabled(void)
{
    return s_cfg.enabled;
}

int schedule_today_wday(void)
{
    int64_t now = standby_time_now();
    if (now <= 0) return -1;
    return epoch_wday(now);
}

const schedule_slot_t *schedule_today_slot(int slot_idx)
{
    if (!s_cfg.enabled) return NULL;
    if (slot_idx < 0 || slot_idx >= SCHED_SLOTS) return NULL;
    int wday = schedule_today_wday();
    if (wday < 0) return NULL;
    const schedule_slot_t *s = &s_cfg.day[wday][slot_idx];
    return (s->deck_id[0]) ? s : NULL;   /* 空槽返回 NULL */
}

int schedule_today_slot_count(void)
{
    if (!s_cfg.enabled) return 0;
    int wday = schedule_today_wday();
    if (wday < 0) return 0;
    int count = 0;
    for (int s = 0; s < SCHED_SLOTS; s++)
        if (s_cfg.day[wday][s].deck_id[0]) count++;
    return count;
}

/* ---- 自动激活核心 ---- */

int schedule_try_activate(void)
{
    /* 1. 检查课程表是否启用 */
    if (!s_cfg.enabled) return 1;

    /* 2. 获取当前日期 */
    int64_t now = standby_time_now();
    if (now <= 0) return 1;   /* 时钟未同步，不激活 */
    int32_t today = epoch_ymd(now);
    if (today == 0) return 1;

    /* 3. 同日不重复触发 */
    if (today == s_state.last_ymd) return 1;

    /* 4. 跨日重置手动干预标记 */
    s_state.manual_override = false;

    /* 5. 手动干预后当日不再自动切换（跨日已重置，此处判断今日是否
     *    曾手动过——跨日后首次进入此路径不会命中） */
    if (s_state.manual_override) {
        s_state.last_ymd = today;
        state_save();
        return 1;
    }

    /* 6. 获取今天星期几 */
    int wday = epoch_wday(now);
    if (wday < 0) return 1;

    /* 7. 遍历今日槽位，找首个未达标项 */
    for (int s = 0; s < SCHED_SLOTS; s++) {
        schedule_slot_t *slot = &s_cfg.day[wday][s];
        if (!slot->deck_id[0]) continue;   /* 空槽跳过 */

        /* 卡组是否存在 */
        int idx = deck_manager_find_index(slot->deck_id);
        if (idx < 0) {
            LOG_W("schedule: deck %s not found, skip slot %d",
                  slot->deck_id, s);
            continue;
        }

        /* 检查该卡组今日是否已达标 */
        int done = learning_state_deck_today_new(slot->deck_id);
        int goal = slot->goal > 0 ? slot->goal
                                  : daily_plan_goal_deck(slot->deck_id);
        if (done >= goal) continue;   /* 已达标，看下一槽 */

        /* 找到首个未达标槽位 → 切换卡组 */
        if (!deck_flow_switch(idx)) {
            LOG_E("schedule: deck_flow_switch(%s) failed", slot->deck_id);
            continue;
        }

        /* 记录激活状态 */
        s_state.last_ymd = today;
        s_state.last_slot = (int8_t)s;
        snprintf(s_state.last_deck, sizeof(s_state.last_deck),
                 "%s", slot->deck_id);
        s_state.manual_override = false;
        state_save();

        LOG_I("schedule activated: day=%d slot=%d deck=%s goal=%d",
              wday, s, slot->deck_id, goal);
        return 0;
    }

    /* 所有槽位均已达标或无有效槽位 */
    s_state.last_ymd = today;
    s_state.last_slot = -1;
    s_state.manual_override = false;
    state_save();
    return -1;
}

void schedule_on_manual_switch(void)
{
    int64_t now = standby_time_now();
    if (now <= 0) return;
    int32_t today = epoch_ymd(now);
    if (today == 0) return;

    /* 同日且今日有过自动激活（last_ymd 匹配）→ 标记手动干预 */
    if (today == s_state.last_ymd && s_state.last_slot >= 0) {
        s_state.manual_override = true;
        state_save();
        LOG_I("schedule: manual override for today");
    }
}

/* ---- 显示课表数据读写（LAN Web 编辑器用） ---- */

const schedule_display_t *schedule_display_cfg(void)
{
    return &s_disp;
}

schedule_display_t *schedule_display_cfg_mut(void)
{
    return &s_disp;
}

void schedule_display_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY_SCHED_DISP, &s_disp, sizeof(s_disp));
    nvs_commit(h);
    nvs_close(h);
    LOG_I("display table saved: rows=%d cols=%d", s_disp.rows, s_disp.cols);
}

bool schedule_display_has_data(void)
{
    return s_disp.enabled && s_disp.rows > 0 && s_disp.cols > 0;
}

/* ============================================================
 * 课程表屏幕渲染（从 NVS 读取显示数据，全屏绘制后 flush）
 * 无数据时显示占位提示
 * ============================================================ */

void schedule_draw_display_table(void)
{
    int sw = epd_gfx_width();
    int sh = epd_gfx_height();

    /* 清屏白底 */
    epd_gfx_fill_screen(EPD_GFX_WHITE);

    /* 无数据时显示占位提示（主提示 LARGE 升主内容级 32px，其余档
     * 20px 现状零变化；副提示 y = 主提示底 + 8——MID 数值巧合与
     * 原 sh/2+8 精确一致） */
    if (!schedule_display_has_data()) {
        const char *msg = "暂无课程表";
        const char *hint = "LAN Web 编辑器设置";
        int lvl = layout_profile_get()->font_lvl_main >= 3 ? 3 : 1;
        int px = cjk_glyph_cell_size(lvl);
        int tw = cjk_text_width(lvl, msg);
        int y0 = sh / 2 - 20;
        cjk_text_draw((sw - tw) / 2, y0, lvl, msg, EPD_GFX_BLACK);
        tw = cjk_text_width(0, hint);
        cjk_text_draw((sw - tw) / 2, y0 + px + 8, 0, hint, EPD_GFX_BLACK);
        epd_power_on();
        epd_gfx_flush();
        epd_power_off();
        LOG_I("display table: no data, placeholder shown");
        return;
    }

    int rows = s_disp.rows;
    int cols = s_disp.cols;

    /* ---- 布局参数（2026-09-08 接 PPI 自动层）----
     * 字级：标题全档顶级；正文/标签 LARGE（font_lvl_main>=3）随主
     * 内容升 32/20px，其余档保持 24/16px（MID 真机基线零变化）；
     * 几何 head_h/label_w/行高上限随 cell 派生。px_title 原 28 系
     * 三级字库时代笔误（2026-09-03 四级化后 level 3 实为 32px，
     * 漏改处——本轮修正垂直居中偏 4px） */
    const layout_profile_t *lp = layout_profile_get();
    const bool big = lp->font_lvl_main >= 3;   /* LARGE 档放大开关 */
    const int title_h  = 36;    /* 标题栏高度（32px 字贴边 2px，全档现状） */
    const int head_h   = big ? 38 : 30;   /* 表头行高 = body cell + 6 */
    const int sep_h    = 4;     /* 上午/下午分隔线高度 */
    const int label_w  = big ? 60 : 44;   /* 节次标签列宽（20px 标签加宽） */
    const int font_title = 3;   /* 32px（全档顶级） */
    const int font_body  = big ? 3 : 2;   /* LARGE 32px / 其余 24px */
    const int font_label = big ? 1 : 0;   /* LARGE 20px / 其余 16px */
    const int px_title   = cjk_glyph_cell_size(font_title);
    const int px_body    = cjk_glyph_cell_size(font_body);
    const int px_label   = cjk_glyph_cell_size(font_label);
    /* 动态计算行高：适配不同行数；上限 = body cell + 8（MID 32 现状，
     * LARGE 40——32px 课名不顶格） */
    int content_h = sh - title_h - head_h;
    int row_h;
    bool has_split = (rows > 4);
    if (has_split) content_h -= sep_h;
    row_h = content_h / rows;
    if (row_h > px_body + 8) row_h = px_body + 8;
    const int day_w = (sw - label_w) / cols;

    /* Y 坐标 */
    int y_head  = title_h;
    int y_am    = title_h + head_h;
    int y_sep, y_pm;
    if (has_split) {
        int am_rows = rows / 2;
        y_sep = y_am + am_rows * row_h;
        y_pm  = y_sep + sep_h;
    } else {
        y_sep = 0;
        y_pm  = y_am;
    }

    /* ---- 标题栏（红底白字） ---- */
    epd_gfx_fill_rect(0, 0, sw, title_h, EPD_GFX_ACCENT);
    {
        const char *title = s_disp.title[0] ? s_disp.title : "课程表";
        int tw = cjk_text_width(font_title, title);
        cjk_text_draw((sw - tw) / 2, (title_h - px_title) / 2,
                      font_title, title, EPD_GFX_WHITE);
    }

    /* ---- 星期表头行（红底白字） ---- */
    epd_gfx_fill_rect(0, y_head, sw, head_h, EPD_GFX_ACCENT);
    for (int d = 0; d < cols; d++) {
        int cx = label_w + d * day_w;
        const char *label = s_disp.days[d][0] ? s_disp.days[d] : "";
        int tw = cjk_text_width(font_body, label);
        cjk_text_draw(cx + (day_w - tw) / 2, y_head + (head_h - px_body) / 2,
                      font_body, label, EPD_GFX_WHITE);
    }

    /* ---- 内容单元格 ---- */
    for (int r = 0; r < rows; r++) {
        int ry;
        if (has_split) {
            int am_rows = rows / 2;
            ry = (r < am_rows) ? (y_am + r * row_h)
                               : (y_pm + (r - am_rows) * row_h);
        } else {
            ry = y_am + r * row_h;
        }

        /* 节次标签（PPI 派生 font_label：LARGE 20px / 其余 16px 紧凑） */
        {
            const char *sl = s_disp.slots[r][0] ? s_disp.slots[r] : "";
            int tw = cjk_text_width(font_label, sl);
            cjk_text_draw(label_w - tw - 2, ry + (row_h - px_label) / 2,
                          font_label, sl, EPD_GFX_BLACK);
        }

        /* 课程内容 */
        for (int d = 0; d < cols; d++) {
            int cx = label_w + d * day_w;
            const char *subj = s_disp.grid[r][d][0] ? s_disp.grid[r][d] : "";
            int tw = cjk_text_width(font_body, subj);
            cjk_text_draw(cx + (day_w - tw) / 2, ry + (row_h - px_body) / 2,
                          font_body, subj, EPD_GFX_BLACK);
        }
    }

    /* ---- 网格线（黑色） ---- */
    int grid_bottom = has_split ? (y_pm + (rows - rows / 2) * row_h)
                                : (y_am + rows * row_h);
    /* 垂直线 */
    for (int c = 0; c <= cols; c++) {
        int x = label_w + c * day_w;
        epd_gfx_draw_vline(x, y_head, grid_bottom - y_head, EPD_GFX_BLACK);
    }
    epd_gfx_draw_vline(label_w, y_head, grid_bottom - y_head, EPD_GFX_BLACK);

    /* 水平线 */
    epd_gfx_draw_hline(0, y_head, sw, EPD_GFX_BLACK);
    for (int r = 0; r <= rows; r++) {
        int ry;
        if (has_split) {
            int am_rows = rows / 2;
            ry = (r <= am_rows) ? (y_am + r * row_h)
                                : (y_pm + (r - am_rows) * row_h);
        } else {
            ry = y_am + r * row_h;
        }
        epd_gfx_draw_hline(0, ry, sw, EPD_GFX_BLACK);
    }

    /* 上午/下午分隔（加粗红线） */
    if (has_split) {
        for (int i = 0; i < sep_h; i++)
            epd_gfx_draw_hline(0, y_sep + i, sw, EPD_GFX_ACCENT);
    }

    /* 外边框 */
    epd_gfx_draw_rect(0, y_head, sw, grid_bottom - y_head, EPD_GFX_BLACK);

    /* 推送到屏幕 */
    epd_power_on();
    epd_gfx_flush();
    epd_power_off();
    LOG_I("display table rendered: %dx%d body=%dpx label=%dpx",
          rows, cols, px_body, px_label);
}

/* 保留旧测试入口（兼容） */
void schedule_draw_test_table(void)
{
    schedule_draw_display_table();
}

/* ============================================================
 * 课程表页面路由（v1.6 修复：按任意键进入学习模式）
 * ============================================================ */

/* 课程表页面按键处理：按任意键弹出进入学习模式 */
static bool schedule_page_on_button(nav_key_t id, button_event_t event)
{
    (void)id;
    (void)event;
    /* 任意按键退出课程表，进入学习模式 */
    haptic_event(HAPTIC_KEYPRESS);
    return false;   /* false = 请求退出编排 */
}

/* 课程表页面入栈回调：渲染课程表首帧 */
static void schedule_page_enter(void)
{
    schedule_draw_display_table();
}

/* 课程表页面定义（page_router 协议） */
const page_t g_schedule_page = {
    .name = "schedule",
    .render = schedule_draw_display_table,
    .on_button = schedule_page_on_button,
    .enter = schedule_page_enter,
    .exit = NULL,
    .owns_display = true,   /* 课程表独占整屏 */
};
