/**
 * @file menu_ui.c
 * @brief 快捷菜单 UI 实现（设计见 docs/MENU_DESIGN.md）
 *
 * 三段式列表页（标题栏/反选高亮列表/提示栏 TINY 省略），列表范式
 * 复用 wifi_config_ui draw_list_page(partial)（2026-08 真机验证）；
 * CJK 标签 16px 点阵（FreeSans 无汉字），右侧徽标混排（ASCII 走
 * FreeSans 基线坐标 / 中文走点阵顶左坐标，两套语义经 draw_badge 统一）。
 * 主列表分组化（2026-08-24，O4）：[学习]/[同步]/[系统] 三组标题行
 * （同按键说明页组头样式，16px 小字不可选中，光标循环跳过）。
 *
 * v1.4 宫格视图（2026-09-07，§12）：主列表双视图共存——列表（默认）/
 * 宫格（图标上标签下，组头独占一行），长按 SET 即时切换即存 NVS
 * （set_menuview，设置页第 13 行同键双入口）；两视图共享线性索引
 * s_sel（切换时光标保持），宫格四向导航（左/右=±1、上/下=±行，
 * 组头跳过）；徽标在格内放不下→选中项徽标改提示栏「标签 · 徽标」
 * 详情显示（body 内重绘：差分局刷下内容不变零成本，随光标更新
 * 天然自洽）；TINY 档强制列表（宫格不开放，短震反馈不生效）。
 *
 * 几何全运行期派生（MU_* 宏 + layout_profile 档位，零特判）：
 *   MID 416x240 项高 44 可见 4 / SMALL 264x176 项高 36 可见 3 /
 *   TINY 122x250|128x296 项高 28 可见 8|9、提示栏省略、CJK 徽标省略、
 *   INFO 页裁至 4 行短值项（IP/PSRAM 长值 122px 宽放不下，bring-up 再调）。
 *   按键说明页同理：值列宽 TINY 档不足，bring-up 后改单列两行/键。
 *   宫格几何（§12.2 实施修订）：列数格宽下限鉀 2~4 列（MID 横 4 列、
 *   MID 竖与 SMALL 2 列——设计稿 SMALL 3 列 88px 格宽放不下 80px
 *   5 字标签，实施改 2 列）、格高/标签级 2026-09-08 PPI 接入：标签
 *   随 hint 级（LARGE 24px / 其余 16px 零变化），几何随 cell 派生。
 *
 * 字号按档位派生（2026-08-23 真机反馈 16px 偏小）：主内容（列表/标题/
 * 模式页/INFO/按键说明）MID/SMALL 用 20px 点阵、TINY 16px；ASCII 徽标
 * FreeSans size 2|1；提示栏维持 16px 辅助小字。
 *
 * 刷新策略（抄 wifi_config_ui 惯例）：进入/换页全刷；光标移动清列表区
 * 重绘 + 单遍局刷（~350ms）；局刷计数达阈值（mu_partial_threshold，
 * desc 基准×菜单系数 400：OPM/WFT 16、DEPG 32、E042BW 64）经
 * refresh_scheduler 升级全刷保养；进入/换页全刷后计数归零
 * （refresh_notify_full_done，LAN 直刷先例），避免带旧账提前触发
 * 保养闪烁；三色屏 partial_enabled=false 由 epd_gfx_flush_window
 * _passes 内部自动降级全刷，无需特判。
 */
#include "menu_ui.h"
#include "page_router.h" /* T1.4：g_menu_ui_page/覆盖层栈（渲染恢复经 render_top） */
#include "debug_log.h"
#include "epd_driver.h"
#include "cjk_text.h"
#include "cjk_font.h"    /* cjk_glyph_cell_size（MU_HINT_LVL 垂直居中，
                          * 2026-09-08 字号派生升级引入） */
#include "layout_profile.h"
#include "refresh_scheduler.h"
#include "haptic.h"
#include "wifi_manager.h"
#include "wifi_config_ui.h"
#include "lan_display_server.h"
#include "audio_sync.h"     /* P0C：音频同步徽标/后台任务启动 */
#include "learning_state.h"
#include "daily_plan.h"  /* v1.2 T2.4：今日行 n/goal 配额显示 */
#include "settings_ui.h" /* v1.2 T2.5：设置页覆盖层入口（二期位） */
#include "max17048.h"  /* v1.2 T2.6：设备信息页电量行（I2C 复用） */
#include "menu_icons.h" /* 1bit 剪影图标前缀（gen_menu_icons.py 生成） */
#include "deck_manager.h" /* v1.3 T3.1：词书卡组（扫描/活跃徽标） */
/* v1.3 T3.1：词书切换编排（main.cpp 导出，quiz_flow_start 同款先例） */
extern bool deck_flow_switch(int idx);
#include "study_mode_machine.h"
#include "word_parser.h"
#include "browse_mode.h"   /* 教材目录三级视图（2026-08-28 设计） */
#include "quiz_ui.h"      /* T1.2：快速测验视图（quiz_ui_start） */
#include "voice_search.h" /* 语音查词状态机（同设计） */
#include "chat_mode.h"    /* A1：chat_request_t（对话二级页确认组包） */
#include "sync_client.h"  /* A3：对话周报拉取（chat-review 端点） */
#include "shortcut_map.h" /* 2026-09-03：按键说明页学习页长按列动态化 */
#include "ui_stamp.h"     /* 2026-09-04：墨封当前词落印动画 */
#include "word_card_ui.h" /* 2026-09-04：菜单退出强制全刷（ui_force_full_refresh_next） */
#include "book_shelf.h"   /* 2026-09-05 阅读器增强：我的书架 */
#include "schedule.h"     /* v1.6：课程表（周计划编排与自动激活） */

#include "freertos/FreeRTOS.h"   /* A3：周报拉取一次性任务 */
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"   /* INFO 页 PSRAM 查询 */

#include <string.h>
#include <stdio.h>

static const char *TAG = "MENU_UI";

/* main.cpp 导出（study_mode_machine.c 引用 ui_render_word 同款先例） */
extern const char *fw_version(void);
/* T1.2：quiz_flow_start 迁 quiz_ui.c 或 quiz_ui_start（quiz_ui.h） */

/* ---- 几何派生（MENU_DESIGN §4.2，全档运行期） ---- */
#define MU_TINY     (layout_profile_get()->kind == LAYOUT_TINY)
#define MU_TITLE_H  (layout_profile_get()->status_h)       /* 对齐 UI_STATUS_H（T1.5 档位参数表） */
#define MU_ITEM_H   (layout_profile_get()->item_h)
#define MU_FONT_H   (layout_profile_get()->font_px_main)   /* 主内容字号（T1.5） */
#define MU_FONT_LVL (layout_profile_get()->font_lvl_main) /* cjk_text level（T1.5） */
#define MU_FONT_ASC (layout_profile_get()->ascii_size_main) /* ASCII 徽标 FreeSans size（T1.5） */
#define MU_HINT_LVL   (layout_profile_get()->font_lvl_main > 0 \
                        ? layout_profile_get()->font_lvl_main - 1 : 0)
                        /* 提示/详情栏字号级：主内容级 -1（2026-09-08
                         * LARGE 定校主内容升 3 后提示栏 16→24px 随升；
                         * 其余档 1-1=0 视觉零变化；TINY 提示栏省略不受影响）*/
#define MU_HINT_H   (layout_profile_get()->hint_h)         /* TINY 省略提示栏（T1.5） */
#define MU_LIST_TOP (MU_TITLE_H + 2)
#define MU_LIST_H   (epd_gfx_height() - MU_TITLE_H - MU_HINT_H)
#define MU_VISIBLE  (MU_LIST_H / MU_ITEM_H)   /* MID=4/SMALL=3/TINY 122x250=8、128x296=9 */
#define MU_MARGIN_X (layout_profile_get()->margin_x)       /* T1.5 档位参数表 */
#define MU_ITEM_W   (epd_gfx_width() - 2 * MU_MARGIN_X)
#define MU_SB_W     4    /* 滚动条宽 */
#define MU_LABEL_W  88   /* INFO 页标签列宽（「收藏/错词」=72px 余量） */
#define MU_KEYS_LBL_W 56 /* 按键说明页键名列宽（20px 档「上/下」=50px） */
#define MU_INFO_LH  (layout_profile_get()->info_lh)  /* INFO/按键说明行高（T1.5） */
#define MU_INFO_ROWS 5                     /* INFO 每页行数（v1.2 T2.6 设备页加电量行 4→5；TINY 超宽值自然截断，bring-up 再调） */

/* ---- 刷新策略 ---- */
/* T1.7 同款公式化（wifi_partial_threshold 惯例）：阈值 =
 * desc.partial_count_full_refresh × 菜单系数（profile.partial_menu）。
 * 2026-09-04：原硬编码 10 与 desc 脱钩，上下选择 10 次即保养全刷
 * 闪烁打断（用户反馈）；公式化后各屏 16/32/64，配合 draw_flush
 * 全刷计数归零，菜单内保养全刷大幅降频。三色屏 partial_enabled
 * =false 由 flush_window_passes 自动降级，阈值不参与 */
static int mu_partial_threshold(void)
{
    const epd_panel_desc_t *pd = epd_panel_desc();
    int base = (pd && pd->partial_count_full_refresh > 0)
             ? pd->partial_count_full_refresh : 8;
    return base * layout_profile_get()->partial_menu / 100;
}

/* ---- 模块状态（静态零初始化，无 init 无堆分配；~10B） ---- */
typedef enum { MU_PAGE_MAIN = 0, MU_PAGE_MODE, MU_PAGE_DECK, MU_PAGE_INFO,
               MU_PAGE_KEYS, MU_PAGE_VOL, MU_PAGE_CHATSEL, MU_PAGE_SCENARIO,
               MU_PAGE_REVIEW, MU_PAGE_SCHEDULE
} mu_page_t;

typedef struct {
    const char *label;                    /* UTF-8 CJK 标签（组头=组名） */
    bool is_header;                       /* 组头行：不可选中，光标跳过 */
    const uint8_t *icon;                  /* 可选 NULL：20px 剪影前缀（TINY 档省略） */
    void (*badge)(char *buf, size_t n);   /* 可选 NULL：右侧徽标（ASCII 或 UTF-8） */
    void (*activate)(void);               /* 中键确认动作 */
} mu_item_t;

static bool      s_active = false;
static const char *s_hint_override = NULL;  /* 预检失败原因等一次性提示 */
static mu_page_t s_page   = MU_PAGE_MAIN;
static int       s_sel    = 0;   /* 主列表选中（0 基；恒非组头；两视图共享） */
static int       s_off    = 0;   /* 主列表滚动偏移（列表视图，行单位） */
static bool      s_grid   = false; /* v1.4 §12：主列表视图 false=列表/true=宫格 */
static int       s_goff   = 0;     /* v1.4：宫格滚动窗口像素偏移（行高不均） */
static int       s_mode_sel = 0; /* 模式列表选中（进入时预定位当前模式） */
static int       s_deck_sel = 0; /* 词书列表选中（进入时预定位活跃卡组） */
static int       s_keys_page = 0; /* 按键说明页页码 */
static int       s_info_page = 0; /* 设备信息页页码（学习概况/设备信息） */
static int       s_chatsel_sel = 0;  /* AI 对话二级页选中（A1：0 自由/1 翻译/2 场景） */
static int       s_scenario_sel = 0; /* 场景列表选中（s_scenarios 下标） */

/* v1.6 课程表设置页状态（s_sched_line/s_sched_deck 两游标随三级页
 * 实现方案调整废弃，2026-09-08 清理） */
static int       s_sched_day   = 0;  /* 当前编辑星期几（0=周一~6=周日） */
static int       s_sched_slot  = 0;  /* 当前编辑槽位（0~3） */

/* 对话周报页（A3）：拉取一次性任务写入，按键上下文只读；gen 代际计数
 * 防任务渲染串页（退出/重进后旧任务结果丢弃） */
#define MU_REVIEW_LOADING 1
#define MU_REVIEW_OK      2
#define MU_REVIEW_NONE    3   /* 404：周报未生成 */
#define MU_REVIEW_FAIL    4
static chat_review_t s_review;              /* 结构体截断已保屏显安全 */
static int           s_review_state = 0;
static int           s_review_page  = 0;   /* OK 态两页：0 概况/1 建议+复习词 */
static volatile int  s_review_gen   = 0;
static volatile bool s_review_busy  = false;

/* 可选模式中文名（二级列表与主菜单徽标共用） */
static const char *s_mode_labels[4] = { "闪卡", "听写", "复习", "阅读" };

/* AI 对话二级选择（A1）：模式页 3 项 + 场景页 6 项镜像后端
 * ScenarioLibrary（Id 短码两侧同步维护；标签纯 CJK TINY 档可显，
 * 与 chat_request_t.title 同源文案） */
static const char *s_chatsel_labels[3] = { "自由对话", "英中翻译", "场景对话" };

typedef struct {
    const char *id;     /* 后端 scenarioId（ASCII 短码） */
    const char *label;  /* 二级页标签（兼作对话页标题） */
} mu_scenario_t;

static const mu_scenario_t s_scenarios[] = {
    { "food",       "餐厅点餐" },
    { "directions", "问路指路" },
    { "school",     "校园聊天" },
    { "shopping",   "商场购物" },
    { "travel",     "旅行住宿" },
    { "doctor",     "看医生" },
};
#define MU_SCENARIO_COUNT ((int)(sizeof(s_scenarios) / sizeof(s_scenarios[0])))

static const char *mu_mode_label(study_mode_t m)
{
    if (m == MODE_WRONGBOOK || m == MODE_COLLECTION) m = MODE_FLASH;
    return s_mode_labels[m];
}

/* 前向声明（activate 引用 exit/二级页绘制；绘制区引用菜单项表） */
static void menu_ui_exit(void);
static void draw_main(bool partial);
static void draw_mode(bool partial);
static void draw_deck(bool partial);
static void draw_info(bool partial);
static void draw_keys(bool partial);
static void draw_vol(bool partial);
static void draw_chatsel(bool partial);
static void draw_scenario(bool partial);
static void draw_schedule(bool partial);

/* ============================================================
 * 徽标填充（每次重绘现取：均为廉价查询，无缓存失效问题）
 * ============================================================ */

static void badge_collected(char *buf, size_t n)
{
    int c = learning_state_collected_count();
    snprintf(buf, n, c == 0 ? "空" : "%d", c);
}

/* 墨封录徽标（2026-09-04）：镜像收藏计数 */
static void badge_mastered(char *buf, size_t n)
{
    int c = learning_state_mastered_count();
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

/* 词书徽标（v1.3 T3.1）：当前活跃卡组名（中文 wide，TINY 省略先例） */
static void badge_deck(char *buf, size_t n)
{
    snprintf(buf, n, "%s", deck_manager_active_name());
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

/* 音量徽标（2026-08-27）：当前档位纯 ASCII（TINY 档可显） */
static void badge_volume(char *buf, size_t n)
{
    snprintf(buf, n, "%d", settings_volume());
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
    page_router_render_top();
}

/* 墨封当前词（2026-09-04）：菜单自退后对词卡上下文当前词 toggle
 * （菜单进入不改学习态，current_word_index 仍有效）；置位方向播
 * 落印动画（ui_stamp，启封即时返回——不对称设计）；after_master
 * 序列收缩钳位/清空退闪卡；墨封录内 toggle = 启封移出同路径 */
static void act_master(void)
{
    if (study_mode_current() == MODE_READER) {
        /* 阅读模式无当前词（SK_ACT_MASTER 同守卫）：页码当索引会墨封
         * 无关词，长震拒绝不进（菜单保持打开，act_collection 空判同构） */
        haptic_event(HAPTIC_ERROR);
        return;
    }
    menu_ui_exit();
    /* 菜单画面残留：首个不切模式的 act_*——同模式回词卡走局刷不重绘
     * 状态栏，菜单顶栏会残留到下次全刷；强制全刷（wifi 退出同款先例），
     * 落印终结帧也带正确顶栏 */
    ui_force_full_refresh_next();
    int wi = study_mode_current_word_index();
    if (wi < 0) {
        haptic_event(HAPTIC_ERROR);   /* 空词库/空序列防御 */
        page_router_render_top();
        return;
    }
    bool mastered = learning_state_toggle_master(wi);
    haptic_event(HAPTIC_REVIEW);
    if (mastered) ui_stamp_play();
    study_mode_after_master();   /* 序列收缩 + 自动跳转下词 */
    page_router_render_top();
}

/* 墨封录（临时视图，act_collection 同构）：空判长震不进入 */
static void act_mastered_list(void)
{
    if (learning_state_mastered_count() == 0) {
        haptic_event(HAPTIC_ERROR);   /* 空墨封录：长震边界反馈不进入 */
        return;
    }
    menu_ui_exit();
    study_mode_enter_mastered();     /* 计数已预检非零，必成功 */
    page_router_render_top();
}

static void act_modesel(void)
{
    s_page = MU_PAGE_MODE;
    s_mode_sel = study_mode_current();   /* 光标预定位当前模式（=「当前」标记） */
    if (s_mode_sel > 3) s_mode_sel = 0;  /* 临时视图（WRONGBOOK/COLLECTION）回闪卡 */
    draw_mode(false);
}

/* 词书选择（v1.3 T3.1，MENU_DESIGN 二期位兑现）：进入即重扫
 * （SD 可能插入新卡组/文件更新；重扫幂等，NVS 活跃记录不变），
 * 光标预定位当前活跃卡组 */
static void act_deck(void)
{
    deck_manager_scan();
    s_page = MU_PAGE_DECK;
    s_deck_sel = deck_manager_active_index();
    draw_deck(false);
}

/* 前置声明：act_wifi TINY 档重定向引用（定义于下方） */
static void act_portal(void);

static void act_wifi(void)
{
    /* TINY 档重定向 AP 门户（2026-08-25）：屏上全键盘 36px 键宽×9 列
     * =324px 不可行（wifi_config_ui.c 头注「该档均不进入」兑现），
     * 该档配网唯一通道=手机连热点浏览器直传 */
    if (layout_profile_get()->kind == LAYOUT_TINY) {
        act_portal();
        return;
    }
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

/* AI 对话（P2B；A1 二级选择页）：先进模式页（自由/英中翻译/场景
 * 对话），确认后才组包进入；前置预检（Wi-Fi/Key/SD）在
 * study_mode_enter_chat 内，不满足长震回学习页；满足则进入对话
 * 临时视图（首帧全刷由 base_render 的 MODE_CHAT 分流承担） */
static void act_chat(void)
{
    s_page = MU_PAGE_CHATSEL;
    s_chatsel_sel = 0;
    draw_chatsel(false);
}

/* 对话确认进入（A1）：按二级页选择组 chat_request_t（mode/scenario/
 * title；free 留空串=URL 不携 query，与老固件请求逐字节一致）；
 * 「先 enter 后 exit」：预检失败留在二级页提示原因（2026-09-01
 * 真机实测：key 未注册时静默回学习页，用户无从得知原因）；成功则
 * 经 g_chat_page 栈化进入（T2.2：enter=首帧全刷，退出编排内聚
 * chat_mode.c） */
static void chat_enter(int chatsel, int scenario_sel)
{
    chat_request_t req;
    memset(&req, 0, sizeof(req));
    if (chatsel == 1) {
        strlcpy(req.mode, "translate", sizeof(req.mode));
        strlcpy(req.title, "英中翻译", sizeof(req.title));
    } else if (chatsel == 2) {
        strlcpy(req.mode, "scenario", sizeof(req.mode));
        strlcpy(req.scenario, s_scenarios[scenario_sel].id,
                sizeof(req.scenario));
        strlcpy(req.title, s_scenarios[scenario_sel].label,
                sizeof(req.title));
    } else {
        strlcpy(req.title, "自由对话", sizeof(req.title));
    }
    int rc = study_mode_enter_chat(&req);
    if (rc != 0) {
        haptic_event(HAPTIC_ERROR);      /* 边界反馈 + 留页提示原因 */
        s_hint_override =
            rc == 1 ? "无网络 · 先 Wi-Fi 配网" :
            rc == 2 ? "设备未注册 · 联网后自动注册重试" :
            rc == 3 ? "无 SD 卡 · 对话音频需落盘" :
                      "对话任务启动失败 · 重试";
        draw_chatsel(false);
        return;
    }
    menu_ui_exit();
    haptic_event(HAPTIC_MODE);            /* 进入新模式 50ms（先例） */
    page_router_push(&g_chat_page);       /* T2.2 栈化：enter=首帧全刷 */
}

/* A3 前向声明（绘制函数在绘制区，文件序同 chat_enter 使用点先行） */
static void draw_review(bool partial);

/* 周报拉取一次性任务（A3）：按键上下文零 HTTP（chat_mode 触发位同哲学
 * 的一次性版本）；结果写静态区后在任务上下文渲染（chat_mode 任务内
 * set_state→ui_render_chat 先例），gen/页态双重校验丢弃过期渲染 */
static void review_fetch_task(void *arg)
{
    int gen = (int)(intptr_t)arg;
    chat_review_t rv;
    int rc = sync_fetch_chat_review(&rv);
    if (rc == 0) {
        s_review = rv;
        s_review_state = MU_REVIEW_OK;
    } else {
        s_review_state = rc == 1 ? MU_REVIEW_NONE : MU_REVIEW_FAIL;
    }
    s_review_busy = false;
    if (s_active && s_page == MU_PAGE_REVIEW && gen == s_review_gen) {
        s_review_page = 0;
        haptic_event(rc == 0 ? HAPTIC_PASS : HAPTIC_ERROR);
        draw_review(false);           /* 加载帧→结果帧（低频页两次全刷可接受） */
    }
    vTaskDelete(NULL);
}

/* 对话周报（A3）：菜单内只读页（不 exit 菜单）；Wi-Fi 预检失败长震
 * 留主列表，任务在跑时轻反馈；首帧「正在获取」全刷，结果帧任务回画 */
static void act_review(void)
{
    if (!wifi_is_connected()) {
        haptic_event(HAPTIC_ERROR);
        return;
    }
    if (s_review_busy) {              /* 上一拉取未完：轻反馈不重入 */
        haptic_event(HAPTIC_KEYPRESS);
        return;
    }
    s_review_busy = true;
    s_review_gen++;
    s_review_state = MU_REVIEW_LOADING;
    s_review_page = 0;
    s_page = MU_PAGE_REVIEW;
    draw_review(false);
    if (xTaskCreate(review_fetch_task, "chatrev", 4096,
                   (void *)(intptr_t)s_review_gen, 4, NULL) != pdPASS) {
        s_review_busy = false;
        s_review_state = MU_REVIEW_FAIL;
        haptic_event(HAPTIC_ERROR);
        draw_review(false);
    }
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

/* 快速测验（v1.2 T2.3，MENU_DESIGN 二期位）：前置词库 ≥8 在
 * study_mode_enter_quiz 内，不满足长震回学习页；满足则经
 * g_quiz_page 栈化进入（T2.2：enter=quiz_ui_start 自绘首帧+自动播，
 * 与 act_browse 同款双轨） */
static void act_quiz(void)
{
    menu_ui_exit();
    if (!study_mode_enter_quiz()) {
        haptic_event(HAPTIC_ERROR);   /* 词库不足：边界反馈 */
        page_router_render_top();
        return;
    }
    haptic_event(HAPTIC_MODE);        /* 进入新模式 50ms（先例） */
    page_router_push(&g_quiz_page);   /* T2.2 栈化：enter 自绘首帧 */
}

/* 我的书架（2026-09-05 阅读器增强）：菜单自退后推书架覆盖层
 * （无前置条件——空书架也显示占位提示页） */
static void act_bookshelf(void)
{
    menu_ui_exit();
    page_router_push(&g_book_shelf_page);
}

/* v1.6 课程表：进入设置页（一级总览，上/下选天，中=编辑该天） */
static void act_schedule(void)
{
    s_page = MU_PAGE_SCHEDULE;
    s_sched_day = schedule_today_wday();
    if (s_sched_day < 0) s_sched_day = 0;
    s_sched_slot = 0;
    draw_schedule(false);
}

/* 教材目录（2026-08-28 设计 §B3）：前置词库 ≥1 在
 * study_mode_enter_browse 内，不满足长震回学习页；满足则三级视图
 * 清态 + 首帧全刷（T1.4 经 g_browse_page 栈顶 render 承担） */
static void act_browse(void)
{
    menu_ui_exit();
    if (!study_mode_enter_browse()) {
        haptic_event(HAPTIC_ERROR);   /* 空词库：边界反馈 */
        page_router_render_top();
        return;
    }
    haptic_event(HAPTIC_MODE);        /* 进入新模式 50ms（先例） */
    page_router_push(&g_browse_page); /* T1.4 试点：enter=browse_mode_reset；
                                       * 首帧 render_top 走栈顶 render */
    page_router_render_top();
}

/* 语音查词（同设计 §B3）：前置 Wi-Fi/Key 在
 * study_mode_enter_voice_search 内（chat 预检先例），不满足长震回
 * 学习页；满足则状态机清态起任务 + 首帧（MODE_VOICE 分流） */
static void act_voice_search(void)
{
    menu_ui_exit();
    if (!study_mode_enter_voice_search()) {
        haptic_event(HAPTIC_ERROR);   /* 无网/未配 Key：边界反馈 */
        page_router_render_top();
        return;
    }
    haptic_event(HAPTIC_MODE);
    voice_search_reset();
    page_router_render_top();
}

static void act_info(void)
{
    s_page = MU_PAGE_INFO;
    s_info_page = 0;
    draw_info(false);
}

/* 音量调节页（2026-08-27）：菜单内即调即听（上/下 ±10 即时生效，
 * 中键试听当前词），不 exit 菜单；状态与设置页音量行共用
 * （settings_volume_set 单一入口：NVS + es8311） */
static void act_volume(void)
{
    s_page = MU_PAGE_VOL;
    draw_vol(false);
}

/* 设置（v1.2 T2.5，MENU_DESIGN 二期位）：菜单自退后进设置覆盖层
 * （同级语义，退出回学习页由 settings_ui 自理） */
static void act_settings(void)
{
    menu_ui_exit();
    page_router_push(&g_settings_ui_page);   /* T1.4：enter=settings_ui_enter */
}

static void act_keys(void)
{
    s_page = MU_PAGE_KEYS;
    s_keys_page = 0;
    draw_keys(false);
}

/* 分组化 15 行 = 3 组头 + 12 项（2026-08-24，O4；二期设置/词书：
 * 数组追加即扩展点，组头行 label 与按键说明页组头同风格方括号；
 * 2026-08-27 [系统] 组增「音量」置「设置」前：高频直达项前置；
 * 2026-08-28 [学习] 组增「教材目录/语音查词」置收藏后（使用频率
 * 前插，设计 §B3；图标复用词书/对话剪影） */
static const mu_item_t s_items[] = {
    { "[ 学习 ]",  true,  NULL,               NULL,             NULL },
    { "收藏列表",   false, menu_icon_collected, badge_collected,  act_collection },
    { "墨封当前词", false, menu_icon_master,    NULL,             act_master },
    { "墨封录",     false, menu_icon_master,   badge_mastered,   act_mastered_list },
    { "教材目录",   false, menu_icon_decks,    NULL,             act_browse },
    { "语音查词",   false, menu_icon_chat,     NULL,             act_voice_search },
    { "模式选择",   false, menu_icon_modesel,  badge_mode,       act_modesel },
    { "词书选择",   false, menu_icon_decks,    badge_deck,       act_deck },
    { "AI 对话",    false, menu_icon_chat,     NULL,             act_chat },
    { "对话周报",   false, menu_icon_info,     NULL,             act_review },
    { "快速测验",   false, menu_icon_quiz,     NULL,             act_quiz },
    { "我的书架",   false, menu_icon_decks,    NULL,             act_bookshelf },
    { "课程表",     false, menu_icon_settings, NULL,             act_schedule },
    { "[ 同步 ]",  true,  NULL,               NULL,             NULL },
    { "音频同步",   false, menu_icon_audio,    badge_audio_sync, act_audio_sync },
    { "Wi-Fi 配网", false, menu_icon_wifi,     badge_wifi,       act_wifi },
    { "AP 配网门户", false, menu_icon_ap,       NULL,            act_portal },
    { "LAN 接收页", false, menu_icon_lan,      NULL,            act_lan },
    { "[ 系统 ]",  true,  NULL,               NULL,             NULL },
    { "音量",       false, menu_icon_volume,   badge_volume,     act_volume },
    { "设置",       false, menu_icon_settings, NULL,             act_settings },
    { "设备信息",   false, menu_icon_info,     NULL,             act_info },
    { "按键说明",   false, menu_icon_keys,     NULL,             act_keys },
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

/* 单项绘制：反选高亮（黑底白字）+ 左图标前缀 + CJK 标签 + 右徽标；
 * 组头行 16px 小字（与提示栏同级，项字号低一档），永不反选；
 * 图标 TINY 档省略（项高 28 与标签宽度紧张，同中文徽标先例） */
static void draw_item(int idx, int row, const mu_item_t *item)
{
    int y = MU_LIST_TOP + row * MU_ITEM_H;
    int w = MU_ITEM_W - MU_SB_W - 4;   /* 列表主体宽（右侧留滚动条） */
    bool sel = (idx == s_sel);

    if (item->is_header) {
        cjk_text_draw(MU_MARGIN_X + 4, y + (MU_ITEM_H - 16) / 2,
                      0, item->label, EPD_GFX_BLACK);
        return;
    }

    if (sel)
        epd_gfx_fill_rect(MU_MARGIN_X, y, w, MU_ITEM_H - 4, EPD_GFX_BLACK);

    int text_x = MU_MARGIN_X + 4;
    if (item->icon && !MU_TINY) {
        epd_gfx_draw_bitmap(text_x, y + (MU_ITEM_H - MENU_ICON_SZ) / 2,
                            MENU_ICON_SZ, MENU_ICON_SZ, item->icon,
                            sel ? EPD_GFX_WHITE : EPD_GFX_BLACK);
        text_x += MENU_ICON_SZ + 4;   /* 图标右缘与标签间距 */
    }

    cjk_text_draw(text_x, y + (MU_ITEM_H - MU_FONT_H) / 2,
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
                  epd_gfx_height() - MU_HINT_H +
                      (MU_HINT_H - cjk_glyph_cell_size(MU_HINT_LVL)) / 2,
                  MU_HINT_LVL, s_hint_override ? s_hint_override
                                     : "上/下 选择  中 确认  SET 返回",
                  EPD_GFX_BLACK);
}

static void draw_flush(void)
{
    epd_power_on();
    epd_gfx_flush();
    epd_power_off();
    /* 进入/换页真全刷等价清残影，计数归零（LAN 直刷先例）——否则
     * 带着学习页等残留计数，菜单内局刷提前触发保养全刷闪烁 */
    refresh_notify_full_done();
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

/* ---- v1.4 宫格视图（§12；TINY 不开放，menu_view_toggle 拒绝）---- */

#define MU_GRID_HDR_H   (8 + cjk_glyph_cell_size(MU_HINT_LVL))   /* 组头行高：
 * hint 级小字 + 上下 padding（MID/SMALL 24 零变化；LARGE 2026-09-08
 * PPI 接入随 hint 级 16→24px 升 32） */
#define MU_GRID_CELL_H  (20 + MENU_ICON_SZ + cjk_glyph_cell_size(MU_HINT_LVL))  /* 格高：
 * pad8 + 图标20 + 间隙4 + 标签 cell + pad8（MID 56 零变化；LARGE 64，
 * 标签 16→24px 随 hint 级，图标位图固有 20px 不缩放） */
#define MU_GRID_MAX_ROWS 12  /* 行表容量上限：3 列下 3 组头+8 格行=11 行 */

typedef struct {
    int  first;      /* 行首项索引（组头行=组头自身索引） */
    int  count;      /* 行内格数（组头行 0） */
    bool is_header;  /* 组头行（独占一行，不可停驻） */
} mu_grow_t;

/* 列数派生（§12.2 实施修订 + 2026-09-08 PPI 接入）：格宽下限 =
 * hint 级 5 字标签 + padding——MID/SMALL 90（16px×5+10，视觉零
 * 变化）/ LARGE 130（24px×5+10，800 宽 4 列格宽 192 仍富余）；
 * 鉀 2~4（MID 横 4 列、MID 竖与 SMALL 2 列，LARGE 同式 4 列） */
static int grid_cols(void)
{
    int min_w = cjk_glyph_cell_size(MU_HINT_LVL) * 5 + 10;
    int cols = MU_ITEM_W / min_w;
    if (cols < 2) cols = 2;
    if (cols > 4) cols = 4;
    return cols;
}

/* 生成宫格行表：s_items 一维展开为组头行+格行二维（每次移动现算，
 * 22 项遍历成本可忽略；s_items 编译期固定 + cols 会话内恒定） */
static int grid_layout(mu_grow_t *rows, int cols)
{
    int n = 0, i = 0;
    while (i < MU_ITEM_COUNT && n < MU_GRID_MAX_ROWS) {
        if (s_items[i].is_header) {
            rows[n].first = i;
            rows[n].count = 0;
            rows[n].is_header = true;
            n++;
            i++;
        } else {
            int start = i;
            while (i < MU_ITEM_COUNT && !s_items[i].is_header) i++;
            int cnt = i - start;
            for (int off = 0; off < cnt && n < MU_GRID_MAX_ROWS; off += cols) {
                rows[n].first = start + off;
                rows[n].count = (cnt - off < cols) ? cnt - off : cols;
                rows[n].is_header = false;
                n++;
            }
        }
    }
    return n;
}

/* 行高（组头/格行不均匀） */
static int grid_row_h(const mu_grow_t *r)
{
    return r->is_header ? MU_GRID_HDR_H : MU_GRID_CELL_H;
}

/* 单格绘制：图标上/标签下居中，选中=黑底反白（列表反选同族）；
 * 图标 NULL 时标签垂直居中（现表已补齐恒非 NULL，防御未来表维护） */
static void draw_cell(int idx, int cell_x, int row_y, int cell_w)
{
    const mu_item_t *it = &s_items[idx];
    bool sel = (idx == s_sel);
    uint16_t fg = sel ? EPD_GFX_WHITE : EPD_GFX_BLACK;

    if (sel)
        epd_gfx_fill_rect(cell_x + 2, row_y + 2,
                          cell_w - 4, MU_GRID_CELL_H - 4, EPD_GFX_BLACK);

    if (it->icon) {
        epd_gfx_draw_bitmap(cell_x + (cell_w - MENU_ICON_SZ) / 2,
                            row_y + 8, MENU_ICON_SZ, MENU_ICON_SZ,
                            it->icon, fg);
        int tw = cjk_text_width(MU_HINT_LVL, it->label);
        cjk_text_draw(cell_x + (cell_w - tw) / 2,
                      row_y + 8 + MENU_ICON_SZ + 4, MU_HINT_LVL,
                      it->label, fg);
    } else {
        int tw = cjk_text_width(MU_HINT_LVL, it->label);
        cjk_text_draw(cell_x + (cell_w - tw) / 2,
                      row_y + (MU_GRID_CELL_H - cjk_glyph_cell_size(MU_HINT_LVL)) / 2,
                      MU_HINT_LVL, it->label, fg);
    }
}

/* 宫格详情栏（提示栏位置兼「选中项详情」§12.5）：徽标格内放不下→
 * 选中项「标签 · 徽标」在此显示；s_hint_override 优先（预检失败
 * 一次性提示先例）。含分隔线：局刷路径 partial_refresh 的 fill 覆盖
 * 提示区，body 必须自含重绘（差分下内容不变零成本，随光标更新） */
static void draw_grid_detail(void)
{
    if (MU_HINT_H == 0) return;   /* 防御（TINY 不进宫格恒 22） */
    epd_gfx_draw_hline(MU_MARGIN_X, epd_gfx_height() - MU_HINT_H,
                       epd_gfx_width() - 2 * MU_MARGIN_X, EPD_GFX_BLACK);

    const char *text;
    char line[48];
    if (s_hint_override) {
        text = s_hint_override;
    } else {
        const mu_item_t *it = &s_items[s_sel];
        char b[16];
        if (it->badge) it->badge(b, sizeof(b));
        else           b[0] = 0;
        if (b[0]) snprintf(line, sizeof(line), "%s · %s", it->label, b);
        else      snprintf(line, sizeof(line), "%s", it->label);
        text = line;
    }
    cjk_text_draw(MU_MARGIN_X,
                  epd_gfx_height() - MU_HINT_H +
                      (MU_HINT_H - cjk_glyph_cell_size(MU_HINT_LVL)) / 2,
                  MU_HINT_LVL, text, EPD_GFX_BLACK);
}

/* 宫格主体：像素级滑动窗口（选中行驱动，行高不均）+ 可见行绘制；
 * 详情栏纳入 body（局刷窗口含提示区，见 draw_grid_detail 注） */
static void draw_grid_body(void)
{
    int cols = grid_cols();
    mu_grow_t rows[MU_GRID_MAX_ROWS];
    int nrows = grid_layout(rows, cols);
    int cell_w = MU_ITEM_W / cols;

    /* 选中项定位像素顶（窗口跟随：上顶入窗/下底不出窗） */
    int sel_top = 0, y = 0;
    for (int r = 0; r < nrows; r++) {
        if (!rows[r].is_header && s_sel >= rows[r].first &&
            s_sel < rows[r].first + rows[r].count)
            sel_top = y;
        y += grid_row_h(&rows[r]);
    }
    if (sel_top < s_goff) s_goff = sel_top;
    if (sel_top + MU_GRID_CELL_H > s_goff + MU_LIST_H)
        s_goff = sel_top + MU_GRID_CELL_H - MU_LIST_H;
    if (s_goff < 0) s_goff = 0;

    y = -s_goff;
    for (int r = 0; r < nrows; r++) {
        int rh = grid_row_h(&rows[r]);
        if (y + rh > 0 && y < MU_LIST_H) {   /* 窗口裁剪 */
            int ry = MU_LIST_TOP + y;
            if (rows[r].is_header) {
                cjk_text_draw(MU_MARGIN_X + 4,
                              ry + (MU_GRID_HDR_H - cjk_glyph_cell_size(MU_HINT_LVL)) / 2,
                              MU_HINT_LVL, s_items[rows[r].first].label,
                              EPD_GFX_BLACK);
            } else {
                for (int c = 0; c < rows[r].count; c++)
                    draw_cell(rows[r].first + c,
                              MU_MARGIN_X + c * cell_w, ry, cell_w);
            }
        }
        y += rh;
    }

    draw_grid_detail();
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

/* ---- 设备信息页（分页只读，上/下翻页同按键说明页范式；
 *      2026-08-24 分两页：学习概况（今日统计，百词斩「今日进度」
 *      借鉴，learning_state NVS lr_stats）+ 设备信息，进入时一次性
 *      取值全刷，翻页局刷） ---- */

static int info_page_count(void)
{
    return 2;
}

static void draw_info_body(void)
{
    static const char *labels[2][MU_INFO_ROWS] = {
        { "词库", "收藏/错词", "今日进度", "连续学习", "考试倒计时" },
        { "固件版本", "运行时长", "电量", "IP 地址", "PSRAM" },
    };
    char v[MU_INFO_ROWS][40];

    if (s_info_page == 0) {
        snprintf(v[0], sizeof(v[0]), "%d 词", word_parser_get_count());
        snprintf(v[1], sizeof(v[1]), "%d / %d",
                 learning_state_collected_count(),
                 learning_state_wrong_count());
        snprintf(v[2], sizeof(v[2]), "%d/%d 新 %d 复",
                 learning_state_deck_today_new(deck_manager_active_id()),
                 daily_plan_goal(),
                 learning_state_today_reviews());
        snprintf(v[3], sizeof(v[3]), "%d 天", learning_state_streak_days());
        {   /* 考试倒计时（v1.5 T5.5）：未设/已过/未同步统一「--」 */
            int d = exam_days_left();
            if (d > 0) snprintf(v[4], sizeof(v[4]), "%d 天", d);
            else       snprintf(v[4], sizeof(v[4]), "%s", "--");
        }
    } else {
        snprintf(v[0], sizeof(v[0]), "%s", fw_version());
        int64_t up = esp_timer_get_time() / 1000000LL;
        snprintf(v[1], sizeof(v[1]), "%02lld:%02lld:%02lld",
                 (long long)(up / 3600), (long long)(up / 60 % 60),
                 (long long)(up % 60));
        int mV = max17048_voltage_mv();        /* T2.6：不在位/读失败占位 */
        int pct = max17048_percent();
        if (pct >= 0 && mV > 0)
            snprintf(v[2], sizeof(v[2]), "%d%% (%d.%02dV)", pct,
                     mV / 1000, mV % 1000 / 10);
        else
            snprintf(v[2], sizeof(v[2]), "--");
        if (!wifi_get_sta_ip(v[3], sizeof(v[3])))
            snprintf(v[3], sizeof(v[3]), "--");
        snprintf(v[4], sizeof(v[4]), "%.1f/%.1f MB",
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1048576.0,
                 heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1048576.0);
    }

    int rows = s_info_page == 0 ? 5 : MU_INFO_ROWS;   /* 概况页 5 行（T5.5 倒计时行） */
    for (int i = 0; i < rows; i++)
        draw_info_row(i, labels[s_info_page][i], v[i]);
}

static void draw_info(bool partial)
{
    if (partial && !refresh_gfx_before_partial_n(mu_partial_threshold())) {
        partial_refresh(draw_info_body);
        return;
    }
    epd_gfx_fill_screen(EPD_GFX_WHITE);
    draw_title(s_info_page == 0 ? "学习概况" : "设备信息",
               s_info_page + 1, info_page_count());
    draw_info_body();
    draw_hint();
    draw_flush();
}

/* ---- 对话周报页（A3，分页只读同 INFO 页范式）：加载/无周报/失败
 *      单帧；OK 态两页（0 概况 + 周次轮数 / 1 建议与复习词），
 *      wrap 断行按剩余高度自适应行数 —— */

static int review_page_count(void)
{
    return s_review_state == MU_REVIEW_OK ? 2 : 1;
}

static void draw_review_body(void)
{
    int y = MU_LIST_TOP + 4;
    char line[64];

    switch (s_review_state) {
    case MU_REVIEW_LOADING:
        cjk_text_draw(MU_MARGIN_X, y, MU_FONT_LVL, "正在获取周报...", EPD_GFX_BLACK);
        cjk_text_draw(MU_MARGIN_X, y + MU_INFO_LH, MU_FONT_LVL,
                      "约需数秒 · 请稍候", EPD_GFX_BLACK);
        return;
    case MU_REVIEW_NONE:
        cjk_text_draw(MU_MARGIN_X, y, MU_FONT_LVL, "暂无对话周报", EPD_GFX_BLACK);
        cjk_text_draw(MU_MARGIN_X, y + MU_INFO_LH, MU_FONT_LVL,
                      "每周日更新 · 对话满一周可看", EPD_GFX_BLACK);
        return;
    case MU_REVIEW_FAIL:
        cjk_text_draw(MU_MARGIN_X, y, MU_FONT_LVL, "获取失败", EPD_GFX_BLACK);
        cjk_text_draw(MU_MARGIN_X, y + MU_INFO_LH, MU_FONT_LVL,
                      "网络/服务暂不可用 · 稍后再试", EPD_GFX_BLACK);
        return;
    }

    if (s_review_page == 0) {
        snprintf(line, sizeof(line), "周 %s 起 · 对话 %d 轮",
                 s_review.week_start + 5, s_review.turn_count);
        cjk_text_draw(MU_MARGIN_X, y, MU_FONT_LVL, line, EPD_GFX_BLACK);
        y += MU_INFO_LH;
        cjk_text_draw(MU_MARGIN_X, y, MU_FONT_LVL, "概况", EPD_GFX_BLACK);
        y += MU_INFO_LH;
        int max_lines = (MU_LIST_H - MU_INFO_LH * 3) / MU_INFO_LH;
        if (max_lines < 1) max_lines = 1;
        cjk_text_draw_wrap_page(MU_MARGIN_X, y, MU_ITEM_W, MU_FONT_LVL,
                                MU_INFO_LH, max_lines, 0,
                                s_review.summary, EPD_GFX_BLACK);
    } else {
        cjk_text_draw(MU_MARGIN_X, y, MU_FONT_LVL, "练习建议", EPD_GFX_BLACK);
        y += MU_INFO_LH;
        int half = (MU_LIST_H - MU_INFO_LH * 2) / MU_INFO_LH / 2;
        if (half < 1) half = 1;
        cjk_text_draw_wrap_page(MU_MARGIN_X, y, MU_ITEM_W, MU_FONT_LVL,
                                MU_INFO_LH, half, 0,
                                s_review.suggestion, EPD_GFX_BLACK);
        int used = cjk_text_wrap_lines(MU_ITEM_W, MU_FONT_LVL,
                                       s_review.suggestion);
        if (used > half) used = half;
        y += used * MU_INFO_LH + MU_INFO_LH;   /* 段间距一行 */
        cjk_text_draw(MU_MARGIN_X, y, MU_FONT_LVL, "复习词", EPD_GFX_BLACK);
        y += MU_INFO_LH;
        int rest = (MU_LIST_TOP + MU_LIST_H - y) / MU_INFO_LH;
        if (rest < 1) rest = 1;
        cjk_text_draw_wrap_page(MU_MARGIN_X, y, MU_ITEM_W, MU_FONT_LVL,
                                MU_INFO_LH, rest, 0,
                                s_review.words, EPD_GFX_BLACK);
    }
}

static void draw_review(bool partial)
{
    if (partial && !refresh_gfx_before_partial_n(mu_partial_threshold())) {
        partial_refresh(draw_review_body);
        return;
    }
    epd_gfx_fill_screen(EPD_GFX_WHITE);
    draw_title("对话周报",
               s_review_state == MU_REVIEW_OK ? s_review_page + 1 : 0,
               review_page_count());
    draw_review_body();
    draw_hint();
    draw_flush();
}

/* ---- v1.6 课程表设置页（一级总览：总开关 + 7 天滚动列表） ---- */

static const char *s_wday_names[7] = {
    "周一", "周二", "周三", "周四", "周五", "周六", "周日"
};

/* 课程表绘制体：总开关行 + 可见窗口内天数（滚动跟随选中项） */
static void draw_schedule_body(void)
{
    const schedule_cfg_t *cfg = schedule_cfg();
    int x = MU_MARGIN_X + 4;
    int y = MU_LIST_TOP;

    /* 总开关行 */
    {
        char buf[48];
        snprintf(buf, sizeof(buf), "课程表: %s",
                 cfg->enabled ? "开" : "关");
        cjk_text_draw(x, y + (MU_INFO_LH - MU_FONT_H) / 2,
                      MU_FONT_LVL, buf, EPD_GFX_BLACK);
        y += MU_INFO_LH + 4;
    }

    /* 计算可见窗口：保证 s_sched_day 始终在窗口内 */
    int list_h = epd_gfx_height() - y - MU_HINT_H;
    int visible = list_h / MU_ITEM_H;
    if (visible > SCHED_DAYS) visible = SCHED_DAYS;
    if (visible < 1) visible = 1;

    int win_start = 0;
    if (s_sched_day >= visible)
        win_start = s_sched_day - visible + 1;
    if (win_start + visible > SCHED_DAYS)
        win_start = SCHED_DAYS - visible;
    if (win_start < 0) win_start = 0;

    /* 可见天数列表 */
    for (int vi = 0; vi < visible; vi++) {
        int d = win_start + vi;
        char buf[64];
        int slot_count = 0;
        for (int s = 0; s < SCHED_SLOTS; s++)
            if (cfg->day[d][s].deck_id[0]) slot_count++;

        bool is_today = (d == schedule_today_wday());
        bool is_sel   = (d == s_sched_day);

        /* 选中行反白 */
        if (is_sel) {
            epd_gfx_fill_rect(0, y, epd_gfx_width(), MU_ITEM_H,
                              EPD_GFX_BLACK);
        }
        uint16_t fg = is_sel ? EPD_GFX_WHITE : EPD_GFX_BLACK;

        if (slot_count == 0) {
            snprintf(buf, sizeof(buf), "%s%s",
                     s_wday_names[d], is_today ? " (休息)" : "  休息");
        } else {
            char detail[48] = "";
            for (int s = 0; s < SCHED_SLOTS; s++) {
                const schedule_slot_t *slot = &cfg->day[d][s];
                if (!slot->deck_id[0]) continue;
                int idx = deck_manager_find_index(slot->deck_id);
                const char *name = (idx >= 0)
                    ? deck_manager_at(idx)->name : "?";
                char sn[4];
                if ((unsigned char)name[0] >= 0xE0)
                    snprintf(sn, sizeof(sn), "%c%c%c",
                             name[0], name[1], name[2]);
                else if ((unsigned char)name[0] >= 0xC0)
                    snprintf(sn, sizeof(sn), "%c%c",
                             name[0], name[1]);
                else
                    snprintf(sn, sizeof(sn), "%c", name[0]);
                char item[16];
                int goal = slot->goal > 0 ? slot->goal
                    : daily_plan_goal_deck(slot->deck_id);
                snprintf(item, sizeof(item), "%s%d ", sn, goal);
                strlcat(detail, item, sizeof(detail));
            }
            snprintf(buf, sizeof(buf), "%s%s %s",
                     s_wday_names[d], is_today ? "" : "", detail);
        }

        cjk_text_draw(x, y + (MU_ITEM_H - MU_FONT_H) / 2,
                      MU_FONT_LVL, buf, fg);
        y += MU_ITEM_H;
    }
}

static void draw_schedule(bool partial)
{
    (void)partial;   /* 课程表翻页必须全刷（反白行位置变化） */
    epd_gfx_fill_screen(EPD_GFX_WHITE);
    draw_title("课程表", 0, 0);
    draw_schedule_body();
    draw_hint();
    draw_flush();
}

/* ---- 按键说明页（表驱动分页只读，上/下翻页；行文全半角标点=字库安全
 *      先例，组头行 key=NULL 整行居左作小节分隔）----
 * 2026-09-03 学习页组长按列动态化：可定制行（上/下/左/右/SET/RST）
 * text=NULL + nav/short_txt 填槽位与短按段，绘制时经 shortcut_map
 * 组合「短按段 / 长按动作名」（DEFAULT=出厂名），说明页与设置页
 * 值列同词表；中键菜单锚点与其余视图文案恒静态 */

typedef struct {
    const char *key;    /* 键名（NULL = 组头行） */
    const char *text;   /* 「短按 / 长按」两段式（NULL = 动态行） */
    nav_key_t   nav;    /* 动态行长按槽位（text==NULL 时有效） */
    const char *short_txt; /* 动态行短按段（text==NULL 时有效） */
} mu_keyrow_t;

static const mu_keyrow_t s_keys[] = {
    /* 静态行补全 0/NULL 初始化（-Wmissing-field-initializers；
     * 绘制只认 text==NULL 的动态行，nav/short_txt 不被读） */
    { NULL,     "[ 学习页 ]", 0, NULL },
    { "上",     NULL, NAV_UP,    "上一词" },
    { "下",     NULL, NAV_DOWN,  "下一词" },
    { "左",     NULL, NAV_LEFT,  "自评忘记" },
    { "右",     NULL, NAV_RIGHT, "简单·墨封" },  /* 2026-09-04：自评简单联动墨封 */
    { "中",     "发音 / 功能菜单", 0, NULL },   /* 锚点不可定制，恒出厂 */
    { "SET",    NULL, NAV_SET,   "遮蔽" },
    { "RST",    NULL, NAV_RST,   "进设置" },
    { "*/熟",   "收藏/墨封标记", 0, NULL },   /* 星标=收藏，方印=已墨封（2026-09-04） */
    { NULL,     "[ 复习词表 ]", 0, NULL },
    { "上/下",  "选择 · 详情翻义", 0, NULL },
    { "中",     "进详情 · 发音", 0, NULL },
    { "左/右",  "自评出队（详情态回列表）", 0, NULL },
    { "RST",    "进设置", 0, NULL },
    { NULL,     "[ 收藏/错词视图 ]", 0, NULL },
    { "上/下",  "序列内翻词", 0, NULL },
    { "中",     "发音", 0, NULL },
    { "SET",    "遮蔽 / 移出视图", 0, NULL },   /* 收藏=移出，墨封录=启封（同构，2026-09-04） */
    { "RST",    "进设置 / 退出视图", 0, NULL },
    { NULL,     "[ AI 对话 ]", 0, NULL },
    { "上/下",  "选模式 · 选场景", 0, NULL },
    { "中",     "说话·发送·重说", 0, NULL },
    { "RST",    "退出回闪卡", 0, NULL },
    { NULL,     "[ 快速测验 ]", 0, NULL },
    { "上/下",  "移动选项", 0, NULL },
    { "中",     "作答", 0, NULL },
    { "SET",    "跳过（不评分）", 0, NULL },
    { "RST",    "退出回闪卡", 0, NULL },
    { NULL,     "[ 待机页 ]", 0, NULL },
    { "中",     "拉天气 / 功能菜单", 0, NULL },
    { "SET",    "轮换引文", 0, NULL },
    { NULL,     "[ 菜单内 ]", 0, NULL },
    { "上/下",  "移动选择 · 宫格跨行", 0, NULL },
    { "左/右",  "宫格行内移动", 0, NULL },
    { "中",     "确认 / 进入", 0, NULL },
    { "SET",    "返回 / 主层退出", 0, NULL },
    { "SET长",  "切换列表/宫格", 0, NULL },
    { "RST",    "退出回原页面", 0, NULL },
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
            const char *text = s_keys[idx].text;
            char dyn[40];   /* 动态行：短按段(≤12B)+" / "+动作名(≤12B) */
            if (!text) {
                sk_action_t a = shortcut_get(s_keys[idx].nav);
                if (a == SK_ACT_DEFAULT)
                    a = shortcut_factory_action(s_keys[idx].nav);
                snprintf(dyn, sizeof(dyn), "%s / %s",
                         s_keys[idx].short_txt, shortcut_action_name(a));
                text = dyn;
            }
            cjk_text_draw(MU_MARGIN_X + MU_KEYS_LBL_W, y, MU_FONT_LVL,
                          text, EPD_GFX_BLACK);
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
    if (partial && !refresh_gfx_before_partial_n(mu_partial_threshold())) {
        partial_refresh(draw_keys_body);
        return;
    }
    epd_gfx_fill_screen(EPD_GFX_WHITE);
    draw_title("按键说明", s_keys_page + 1, keys_page_count());
    draw_keys_body();
    draw_hint();
    draw_flush();
}

/* ---- 音量调节页（2026-08-27）：中央大字档位 + 比例条，上/下 ±10
 *      即时生效（settings_volume_set：NVS+es8311 单一入口） ---- */

static void draw_vol_body(void)
{
    int w = epd_gfx_width();
    int area_y = MU_TITLE_H;
    int area_h = epd_gfx_height() - MU_TITLE_H - MU_HINT_H;

    /* 档位大字（FreeSans 倍号：TINY 3 / 其余 4；基线在内容区 40% 处） */
    char vtxt[8];
    snprintf(vtxt, sizeof(vtxt), "%d", settings_volume());
    int size = MU_TINY ? 3 : 4;
    int tw, th;
    epd_gfx_text_bounds(vtxt, size, &tw, &th);
    epd_gfx_draw_text((w - tw) / 2, area_y + area_h * 2 / 5,
                      vtxt, EPD_GFX_BLACK, size);

    /* 比例条：外框 + 内填充（0=空框即静音；宽≤60% 屏宽，TINY 也容纳） */
    int bar_w = w * 3 / 5;
    int bar_h = 10;
    int bar_x = (w - bar_w) / 2;
    int bar_y = area_y + area_h * 2 / 5 + 12;
    epd_gfx_draw_rect(bar_x, bar_y, bar_w, bar_h, EPD_GFX_BLACK);
    epd_gfx_fill_rect(bar_x + 2, bar_y + 2,
                      (bar_w - 4) * settings_volume() / 100, bar_h - 4,
                      EPD_GFX_BLACK);
}

static void draw_vol(bool partial)
{
    if (partial && !refresh_gfx_before_partial_n(mu_partial_threshold())) {
        partial_refresh(draw_vol_body);
        return;
    }
    epd_gfx_fill_screen(EPD_GFX_WHITE);
    draw_title("音量", 0, 0);
    draw_vol_body();
    /* 专用提示（通用 draw_hint 文案不贴切；TINY 同款 MU_HINT_H=0 省略） */
    if (MU_HINT_H > 0)
        cjk_text_draw(MU_MARGIN_X,
                      epd_gfx_height() - MU_HINT_H + (MU_HINT_H - 16) / 2,
                      0, "上/下 调节  中 试听  SET 返回", EPD_GFX_BLACK);
    draw_flush();
}

/* ---- 页面绘制入口（partial=true 局刷路径 + 阈值升级全刷） ---- */

static void draw_main(bool partial)
{
    if (partial && !refresh_gfx_before_partial_n(mu_partial_threshold())) {
        partial_refresh(s_grid ? draw_grid_body : draw_main_body);
        return;
    }
    epd_gfx_fill_screen(EPD_GFX_WHITE);
    draw_title("功能菜单", s_sel + 1, MU_ITEM_COUNT);
    if (s_grid) {
        draw_grid_body();   /* 含详情栏（线+文字，§12.5）*/
    } else {
        draw_main_body();
        draw_hint();
    }
    draw_flush();
}

static void draw_mode(bool partial)
{
    if (partial && !refresh_gfx_before_partial_n(mu_partial_threshold())) {
        partial_refresh(draw_mode_body);
        return;
    }
    epd_gfx_fill_screen(EPD_GFX_WHITE);
    draw_title("模式选择", s_mode_sel + 1, 4);
    draw_mode_body();
    draw_hint();
    draw_flush();
}

/* ---- 二级词书列表页（v1.3 T3.1）：镜像模式选择页范式；右侧词条数
 *      徽标（manifest count，0=未标注不显示）；超过一屏时滑动窗口
 *      跟随光标（不画滚动条，选择语义与主列表一致） ---- */
static void draw_deck_body(void)
{
    int total = deck_manager_count();
    int off = s_deck_sel - MU_VISIBLE + 1;   /* 窗口跟随光标（下界钳 0） */
    if (off < 0) off = 0;
    if (total > MU_VISIBLE && off > total - MU_VISIBLE)
        off = total - MU_VISIBLE;

    for (int i = 0; i < MU_VISIBLE; i++) {
        int di = off + i;
        const deck_info_t *d = deck_manager_at(di);
        if (!d) break;
        int y = MU_LIST_TOP + i * MU_ITEM_H;
        bool sel = (di == s_deck_sel);
        if (sel)
            epd_gfx_fill_rect(MU_MARGIN_X, y, MU_ITEM_W, MU_ITEM_H - 4,
                              EPD_GFX_BLACK);
        cjk_text_draw(MU_MARGIN_X + 4, y + (MU_ITEM_H - MU_FONT_H) / 2,
                      MU_FONT_LVL, d->name,
                      sel ? EPD_GFX_WHITE : EPD_GFX_BLACK);
        {   /* 跨科目角标（T5.5）：各组今日 n/goal（纯 ASCII，TINY 可显）；
             * 词条数信息让位——切换前看「哪组没学完」价值更高 */
            char cb[12];
            snprintf(cb, sizeof(cb), "%d/%d",
                     learning_state_deck_today_new(d->id),
                     daily_plan_goal_deck(d->id));
            draw_badge(MU_MARGIN_X + MU_ITEM_W - MU_SB_W - 10, y, cb,
                       sel ? EPD_GFX_WHITE : EPD_GFX_BLACK);
        }
    }
}

static void draw_deck(bool partial)
{
    if (partial && !refresh_gfx_before_partial_n(mu_partial_threshold())) {
        partial_refresh(draw_deck_body);
        return;
    }
    epd_gfx_fill_screen(EPD_GFX_WHITE);
    draw_title("词书选择", s_deck_sel + 1, deck_manager_count());
    draw_deck_body();
    draw_hint();
    draw_flush();
}

/* ---- AI 对话二级页（A1）：模式页 3 项镜像模式选择页范式；场景页
 *      6 项 MID 可见 4 需滑动窗口跟随（词书页同款，不画滚动条） ---- */

static void draw_chatsel_body(void)
{
    for (int i = 0; i < 3 && i < MU_VISIBLE; i++) {
        int y = MU_LIST_TOP + i * MU_ITEM_H;
        bool sel = (i == s_chatsel_sel);
        if (sel)
            epd_gfx_fill_rect(MU_MARGIN_X, y, MU_ITEM_W, MU_ITEM_H - 4,
                              EPD_GFX_BLACK);
        cjk_text_draw(MU_MARGIN_X + 4, y + (MU_ITEM_H - MU_FONT_H) / 2,
                      MU_FONT_LVL,
                      s_chatsel_labels[i], sel ? EPD_GFX_WHITE : EPD_GFX_BLACK);
    }
}

static void draw_chatsel(bool partial)
{
    if (partial && !refresh_gfx_before_partial_n(mu_partial_threshold())) {
        partial_refresh(draw_chatsel_body);
        return;
    }
    epd_gfx_fill_screen(EPD_GFX_WHITE);
    draw_title("AI 对话", s_chatsel_sel + 1, 3);
    draw_chatsel_body();
    draw_hint();
    draw_flush();
}

static void draw_scenario_body(void)
{
    int off = s_scenario_sel - MU_VISIBLE + 1;   /* 窗口跟随光标（下界铺 0） */
    if (off < 0) off = 0;
    if (MU_SCENARIO_COUNT > MU_VISIBLE && off > MU_SCENARIO_COUNT - MU_VISIBLE)
        off = MU_SCENARIO_COUNT - MU_VISIBLE;

    for (int i = 0; i < MU_VISIBLE; i++) {
        int si = off + i;
        if (si >= MU_SCENARIO_COUNT) break;
        int y = MU_LIST_TOP + i * MU_ITEM_H;
        bool sel = (si == s_scenario_sel);
        if (sel)
            epd_gfx_fill_rect(MU_MARGIN_X, y, MU_ITEM_W, MU_ITEM_H - 4,
                              EPD_GFX_BLACK);
        cjk_text_draw(MU_MARGIN_X + 4, y + (MU_ITEM_H - MU_FONT_H) / 2,
                      MU_FONT_LVL, s_scenarios[si].label,
                      sel ? EPD_GFX_WHITE : EPD_GFX_BLACK);
    }
}

static void draw_scenario(bool partial)
{
    if (partial && !refresh_gfx_before_partial_n(mu_partial_threshold())) {
        partial_refresh(draw_scenario_body);
        return;
    }
    epd_gfx_fill_screen(EPD_GFX_WHITE);
    draw_title("场景对话", s_scenario_sel + 1, MU_SCENARIO_COUNT);
    draw_scenario_body();
    draw_hint();
    draw_flush();
}

/* ============================================================
 * 按键处理（激活时独占；长按全部忽略防误触）
 * ============================================================ */

static void menu_ui_exit(void)
{
    page_router_pop_if(&g_menu_ui_page);   /* T1.4：所有退出路径统一
     * 出栈（非栈顶时 NULL 防御；「先 exit 后 enter」纪律不变） */
    s_active = false;
    s_page   = MU_PAGE_MAIN;
    s_sel = s_off = 0;
    s_goff = 0;   /* v1.4：宫格窗口复位（s_grid 不清，下次 enter 重置） */
}

/* 恢复型退出（SET 主菜单层 / RST 任意层级）：exit 后经
 * page_router_render_top 恢复学习页/待机页（模式变化时自然全刷） */
static void menu_ui_exit_restore(void)
{
    menu_ui_exit();
    page_router_render_top();
}

/* 光标移动（循环滚动，组头行跳过不可停驻）+ 滚动窗口跟随 +
 * 局刷重绘 */
static void main_move(int dir)
{
    do {
        s_sel = (s_sel + dir + MU_ITEM_COUNT) % MU_ITEM_COUNT;
    } while (s_items[s_sel].is_header);   /* 表恒有非组头项，无死循环 */
    if (s_sel < s_off) s_off = s_sel;
    if (s_sel >= s_off + MU_VISIBLE) s_off = s_sel - MU_VISIBLE + 1;
    draw_main(true);
}

/* v1.4 §12.3：视图切换（菜单内长按 SET，即改即存 NVS 全刷重排）；
 * s_sel 保持（两视图同一索引空间），滚动偏移按新几何复位重算 */
static void menu_view_toggle(void)
{
    if (MU_TINY) {   /* TINY 强制列表（§12.2 档位核算），短震反馈 */
        haptic_event(HAPTIC_ERROR);
        return;
    }
    s_grid = !s_grid;
    settings_menu_grid_set(s_grid);
    s_goff = 0;   /* 几何已变，窗口复位（draw 内跟随重算） */
    haptic_event(HAPTIC_MODE);
    draw_main(false);   /* 模式重排=全刷（进入/换页惯例） */
    LOG_I("menu view -> %s", s_grid ? "grid" : "list");
}

/* v1.4 §12.3：宫格四向移动——左/右=±1 跳组头（列表 main_move 同款
 * 循环语义）；上/下=±行（目标行同列、列超行尾钳末格，组头行环形
 * 跳过；表恒有非组头项无死循环，main_move 先例） */
static void grid_move(int dx, int dy)
{
    if (dx) {
        do {
            s_sel = (s_sel + dx + MU_ITEM_COUNT) % MU_ITEM_COUNT;
        } while (s_items[s_sel].is_header);
    }
    if (dy) {
        int cols = grid_cols();
        mu_grow_t rows[MU_GRID_MAX_ROWS];
        int nrows = grid_layout(rows, cols);
        int r = 0, c = 0;
        for (int i = 0; i < nrows; i++) {
            if (!rows[i].is_header && s_sel >= rows[i].first &&
                s_sel < rows[i].first + rows[i].count) {
                r = i;
                c = s_sel - rows[i].first;
                break;
            }
        }
        int tr = r;
        do {
            tr = (tr + dy + nrows) % nrows;   /* 环形（列表循环一致） */
        } while (rows[tr].is_header);
        int tc = (c < rows[tr].count) ? c : rows[tr].count - 1;
        s_sel = rows[tr].first + tc;
    }
    draw_main(true);
}

void menu_ui_on_button(nav_key_t id, button_event_t event)
{
    if (!s_active) return;
    if (event == BUTTON_EVENT_LONG_PRESS) {
        /* v1.4 §12.3：主菜单层长按 SET=切换列表/宫格视图；其余长按
         * 全忽略（防误触，原语义不变） */
        if (s_page == MU_PAGE_MAIN && id == NAV_SET) menu_view_toggle();
        return;
    }
    if (event != BUTTON_EVENT_SHORT_PRESS) return;
    s_hint_override = NULL;                        /* 任意按键清一次性提示 */

    switch (s_page) {
    case MU_PAGE_MAIN:
        if (s_grid) {   /* v1.4：宫格四向导航（左/右从忽略升格为移动） */
            switch (id) {
            case NAV_UP:     grid_move(0, -1); break;
            case NAV_DOWN:   grid_move(0, +1); break;
            case NAV_LEFT:   grid_move(-1, 0); break;
            case NAV_RIGHT:  grid_move(+1, 0); break;
            case NAV_CENTER: s_items[s_sel].activate(); break;
            case NAV_SET:    menu_ui_exit_restore(); break;
            case NAV_RST:    menu_ui_exit_restore(); break;
            default: break;
            }
            break;
        }
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
            page_router_render_top();
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

    case MU_PAGE_DECK:
        /* 中=切换（重载编排 deck_flow_switch）；重复选当前=轻反馈 */
        switch (id) {
        case NAV_UP:
            s_deck_sel = (s_deck_sel + deck_manager_count() - 1) %
                         deck_manager_count();
            draw_deck(true);
            break;
        case NAV_DOWN:
            s_deck_sel = (s_deck_sel + 1) % deck_manager_count();
            draw_deck(true);
            break;
        case NAV_CENTER:
            if (s_deck_sel == deck_manager_active_index()) {
                haptic_event(HAPTIC_KEYPRESS);
                break;
            }
            if (deck_flow_switch(s_deck_sel)) {
                haptic_event(HAPTIC_MODE);
                menu_ui_exit();
                page_router_render_top();
            } else {
                haptic_event(HAPTIC_ERROR);   /* 文件缺失/重载失败 */
            }
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

    case MU_PAGE_INFO:
        /* 分页只读：上/下翻页（循环），中=下一页，SET 返回，RST 退出 */
        switch (id) {
        case NAV_UP:
            s_info_page = (s_info_page + info_page_count() - 1) % info_page_count();
            draw_info(true);
            break;
        case NAV_DOWN:
        case NAV_CENTER:
            s_info_page = (s_info_page + 1) % info_page_count();
            draw_info(true);
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

case MU_PAGE_VOL:
        /* 音量调节（2026-08-27）：上/下 ±10 即时生效重绘，中=试听
         * 当前词（study_mode speak 语义动作复用，纯拼路径+异步入队
         * 不动菜单/学习页状态），SET 返回主列表，RST 退出菜单 */
        switch (id) {
        case NAV_UP:
            settings_volume_set(settings_volume() + 10);
            draw_vol(true);
            break;
        case NAV_DOWN:
            settings_volume_set(settings_volume() - 10);
            draw_vol(true);
            break;
        case NAV_CENTER:
            study_mode_handle_action(3);
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

    case MU_PAGE_CHATSEL:
        /* AI 对话模式二级页（A1）：上/下循环移动，中=确认（场景项
         * 转场景列表页），SET 返回主菜单，RST 退出 */
        switch (id) {
        case NAV_UP:
            s_chatsel_sel = (s_chatsel_sel + 2) % 3;
            draw_chatsel(true);
            break;
        case NAV_DOWN:
            s_chatsel_sel = (s_chatsel_sel + 1) % 3;
            draw_chatsel(true);
            break;
        case NAV_CENTER:
            if (s_chatsel_sel == 2) {
                s_page = MU_PAGE_SCENARIO;
                s_scenario_sel = 0;
                draw_scenario(false);
            } else {
                chat_enter(s_chatsel_sel, 0);
            }
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

    case MU_PAGE_SCENARIO:
        /* 场景列表（A1）：上/下循环（6 项滑动窗口），中=进入场景
         * 对话，SET 返回上级模式页，RST 退出 */
        switch (id) {
        case NAV_UP:
            s_scenario_sel = (s_scenario_sel + MU_SCENARIO_COUNT - 1) %
                             MU_SCENARIO_COUNT;
            draw_scenario(true);
            break;
        case NAV_DOWN:
            s_scenario_sel = (s_scenario_sel + 1) % MU_SCENARIO_COUNT;
            draw_scenario(true);
            break;
        case NAV_CENTER:
            chat_enter(2, s_scenario_sel);
            break;
        case NAV_SET:
            s_page = MU_PAGE_CHATSEL;
            draw_chatsel(false);
            break;
        case NAV_RST:
            menu_ui_exit_restore();
            break;
        default: break;
        }
        break;

    case MU_PAGE_REVIEW:
        /* 对话周报页（A3）：只读——OK 态上/下（中）翻页，加载/空/败
         * 态按键忽略；SET 返回主列表，RST 退出（INFO 页同款） */
        switch (id) {
        case NAV_UP:
            if (s_review_state == MU_REVIEW_OK && review_page_count() > 1) {
                s_review_page = (s_review_page + review_page_count() - 1) %
                                review_page_count();
                draw_review(true);
            }
            break;
        case NAV_DOWN:
        case NAV_CENTER:
            if (s_review_state == MU_REVIEW_OK && review_page_count() > 1) {
                s_review_page = (s_review_page + 1) % review_page_count();
                draw_review(true);
            }
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

    case MU_PAGE_SCHEDULE:
        /* v1.6 课程表设置页：上/下选天，中=切换总开关，SET 返回主菜单，
         * RST 退出（一级页简单交互；二级编辑随后续迭代扩展） */
        switch (id) {
        case NAV_UP:
            s_sched_day = (s_sched_day + SCHED_DAYS - 1) % SCHED_DAYS;
            draw_schedule(true);
            break;
        case NAV_DOWN:
            s_sched_day = (s_sched_day + 1) % SCHED_DAYS;
            draw_schedule(true);
            break;
        case NAV_CENTER:
        {
            /* 中键：切换课程表总开关 */
            schedule_cfg_t *cfg = schedule_cfg_mut();
            cfg->enabled = !cfg->enabled;
            schedule_save();
            haptic_event(HAPTIC_PASS);
            draw_schedule(true);
            break;
        }
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
    /* 首项为组头：定位首个可选项（表首组头后恒有实项） */
    for (s_sel = 0; s_sel < MU_ITEM_COUNT - 1 && s_items[s_sel].is_header;
         s_sel++) {}
    s_off = 0;
    s_goff = 0;
    /* v1.4 §12.4：NVS 视图偏好定初始视图；TINY 档强制列表（宫格
     * 不开放，设置页该档仅存偏好） */
    s_grid = !MU_TINY && settings_menu_grid();
    haptic_event(HAPTIC_MODE);   /* 进入菜单 50ms（对齐模式切换/错词本） */
    draw_main(false);
    LOG_I("menu entered (%s)", s_grid ? "grid" : "list");
}

/* T1.4 页面协议：enter=menu_ui_enter（幂等+触觉+首帧自绘）；
 * exit 无（清态统一在 menu_ui_exit，pop_if 由其调用，置 NULL 防双重）；
 * render 无（栈顶期间整页重绘不可达，模块自管局刷） */
static bool menu_page_on_button(nav_key_t id, button_event_t event)
{
    menu_ui_on_button(id, event);
    return true;   /* 顶层覆盖层总消费（语义不变） */
}

const page_t g_menu_ui_page = { "menu", NULL, menu_page_on_button,
                               menu_ui_enter, NULL, true };

/* 黄金帧动态区域 mask（T3.2）：列表视图=主列表徽标列（badge 右对齐
 * 绘制，几何与 draw_item/draw_badge 同源；宽取徽标最大值：中文 3 字
 * 点阵+余量，TINY 档仅 ASCII 徽标取 40）——收藏数/模式名/Wi-Fi
 * 状态/音频同步数/音量为运行期动态，差异不参与基线比对；
 * v1.4 宫格视图：反白选中格随光标位置不定 + 详情栏动态 → 列表区
 * 整区（标题栏下到屏底）均为动态区（自检 env NVS 空默认列表，
 * 此分支仅 set_menuview=1 时触达） */
int menu_ui_golden_mask(int (*out)[4], int max)
{
    int n = 0;
    if (n < max) {
        if (s_grid) {
            out[n][0] = 0;
            out[n][1] = MU_TITLE_H;
            out[n][2] = epd_gfx_width();
            out[n][3] = epd_gfx_height() - MU_TITLE_H;
        } else {
            int w = MU_ITEM_W - MU_SB_W - 4;            /* 列表主体宽 */
            int bw = MU_TINY ? 40 : (MU_FONT_H * 3 + 8);
            int x = MU_MARGIN_X + w - 6 - bw;           /* badge 左缘（right_x 同源） */
            if (x < 0) x = 0;
            out[n][0] = x;
            out[n][1] = MU_LIST_TOP;
            out[n][2] = bw;
            out[n][3] = MU_VISIBLE * MU_ITEM_H;        /* 列表可见区 */
        }
        n++;
    }
    return n;
}
