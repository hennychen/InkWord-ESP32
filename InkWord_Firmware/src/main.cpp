/**
 * @file main.cpp
 * @brief 应用主入口 (Task F-20)
 *
 * 启动流程：日志 -> NVS -> 存储(SD) -> 屏幕 -> 音频 -> 按键 ->
 *          WiFi -> 词库 -> 进入上次模式 -> 主循环（事件驱动）。
 *
 * 五向导航按键映射（2026-08 取代 6 独立按键；SET/RST 侧键同月接入）：
 *   上 短按=上一条（释义多页时先翻上一释义页）/ 长按=清残影全刷；
 *   下 短按=下一条（释义多页时先翻下一释义页）/ 长按=切换学习模式；
 *   中 短按=发音 / 长按=进入功能菜单（快捷菜单 menu_ui：收藏/模式/
 *        配网/门户/LAN/设备信息；Wi-Fi 配网降为菜单项，2026-08-23；
 *        栈串联制下菜单入栈，子功能页退出回菜单，2026-09-08）；
 *   左 短按=自评「忘记」Q1（SM-2 质量分 1：连错+1，>0 入错词本）/ 长按=进入 AP 直连/配网门户
 *        （手机连 InkWord-Setup 热点直传，绕开路由器隔离；栈页任意键
 *        退出回上级）；
 *   右 短按=自评「简单」Q5（SM-2 质量分 5：连错清零，错词本中移出）/ 长按=进入 LAN 接收页（同网浏览器直传，栈页任意键退出回上级）；
 *   SET 短按=遮蔽/揭晓释义（闪卡自测；待机页=轮换下一条引文）/ 长按=收藏/取消当前词（左栏 * 标记；收藏视图内=移出序列）；
 *   RST 短按=回到当前模式第一条 / 长按=临时视图进出（错词本/收藏/墨封
 *        录栈页，视图内=退出回上级，否则进错词本）；墨封=已熟练标记
 *        （菜单「墨封当前词」/快捷键 SK_ACT_MASTER，置位播落印动画，
 *        闪卡/听写序列过滤；墨封录内 SET 长按=启封移出）。
 * 上述长按动作出厂映射可由用户改绑（2026-09-03 shortcut_map：设置页
 *  「快捷键」子模式，上/下/左/右/SET/RST 六槽位；守卫——中键菜单锚点、
 *  错词本/收藏/墨封录视图内 RST 退出与 SET 移出收藏/启封不可覆盖，见
 *  shortcut_try_long）。
 *
 * 阅读模式（P3，长按下循环切换进入）：上/下=翻页，左/右=字号缩放
 * （16/20/24px 三级循环，按当前页首字符就近保持阅读位置），RST=回
 * 第一页；中/SET 短按与词相关长按（收藏）不适用；进度自动保存
 * （NVS rd_*）。
 *
 * 无词库待机页（词库为空时默认显示，见 standby_page.c）：
 *   时钟/日历/天气整页；待机态长按语义与学习页一致（功能菜单/清残影/
 *   门户/LAN），短按中=立即拉取天气，其余短按忽略。
 *
 * 深睡与定时唤醒（P5，power_manager.c）：无操作 10 分钟入睡（引文轮换/
 * 后台心跳随交互模式冻结，墨水屏驻留末帧零功耗）；中键唤醒恢复交互
 * （自治钟 RTC 慢钟差分恢复 + 联网 HTTP Date 校准兜底）；RTC TIMER
 * 每 2h 静默心跳会话（Wi-Fi 快连 → 校时 → 上报/心跳/OTA → 回睡，
 * 全程不碰屏）。唤醒即重启，setup 最早期按唤醒原因分流。
 */
#include <Arduino.h>

#include "debug_log.h"
#include "gpio_config.h"
#include "inkword_features.h" /* 开源通用化 Phase 1：功能裁剪宏权威
                                * （离线档五轴=0，段内 #if 包裹见各处） */
#include "epd_driver.h"
#include "audio_player.h"
#include "es8311.h"        /* 2026-08-27 音量恢复：启动后 NVS 镜像同步进 codec 驱动状态 */
#include "button_handler.h"
#include "haptic.h"
#include "ui_sfx.h"     /* T1.6 提示音：按键确认/自评/模式/边界 */
#include "quiz_ui.h"     /* T1.2：快速测验视图（自本文件迁出，修 A1；
                         * 出题核心 quiz_session 纯核由 quiz_ui.c 引用） */
#include "review_ui.h"   /* T1.3：复习词表视图（自本文件迁出，修 A1） */
#include "word_card_ui.h" /* P2 拆分：学习页渲染族（自本文件迁出；
                            * ui_render_word/ui_mean_page_step 等经此声明 */
#include "word_view_page.h" /* 架构拆分 2026-09-17：词卡通用按键路由
                              * （自本文件迁出至 word_view_page.c） */
#include "storage_manager.h"
#include "refresh_scheduler.h"
#include "word_parser.h"
#include "word_loader.h"  /* T1.3：词库装载链路（自本文件迁出，修 A1；
                         * setup 步骤 6 与 deck_flow_switch 切书共用） */
#include "cjk_text.h"     /* 词卡释义/tag 中文点阵混排（P3 字库资产） */
#include "cjk_font_sd.h"  /* v1.4 T4.5：SD 卡组子集字库级联装载 */
#include "layout_profile.h" /* 布局档位：SMALL 单列 / MID 双栏分档（§8.1） */
#include "srs_engine.h"
#include "learning_state.h"
#include "daily_plan.h"   /* v1.2 T2.4：每日目标量与今日任务判据 */
#include "settings_ui.h"  /* v1.2 T2.5：设置覆盖层 + 发音/震动/字号门控 */
#include "max17048.h"    /* v1.2 T2.6：电量计（I2C 复用 38/39，实数替换占位） */
#include "deck_manager.h" /* v1.3 T3.1：词书卡组（manifest 扫描/切换） */
#include "card_layout.h" /* v1.4 T4.3：卡组版式分派（qa/poem） */
#include "study_mode_machine.h"
#include "chat_mode.h"    /* P2B：AI 对话模式（MODE_CHAT 按键转发/屏显） */
#include "chat_ui.h"      /* T1.3：AI 对话屏显（自本文件迁出，修 A1） */
#include "catalog_index.h" /* 教材目录索引（browse 数据源，词库装载尾部构建） */
#include "browse_mode.h"   /* 教材目录浏览（MODE_BROWSE 三级目录临时视图） */
#include "voice_search.h"  /* AI 语音查词（MODE_VOICE 四态临时视图） */
#include "wifi_manager.h"
#include "wifi_config_ui.h"
#include "menu_ui.h"      /* 快捷菜单（功能菜单，长按中进入） */
#include "page_router.h" /* T1.4 页面路由：base + 覆盖层栈（page_t 协议） */
#include "sync_session.h" /* P2 拆分：云端同步会话编排（自本文件迁出；
                         * 凭据恢复/后台任务/静默心跳会话经此声明） */
#include "ota_manager.h"
#include "lan_display_server.h"
#include "standby_page.h"
#include "screen_aging_test.h"  /* 屏幕老化诊断测试（2026-09-10） */
#include "reader_engine.h"   /* 阅读模式（P3）：书分页/字号/进度 */
#include "ble_provision.h"
#include "power_manager.h"   /* P5 深睡/唤醒分流与入睡检查 */
#include "shortcut_map.h" /* 2026-09-03 用户自定义长按快捷键（六槽位） */
#include "ui_stamp.h"    /* 2026-09-04：墨封落印动画 */
#include "book_shelf.h"  /* 2026-09-05 阅读器增强：我的书架 */
#include "reader_menu.h"  /* 2026-09-05 阅读器增强：阅读器菜单 */
#include "bookmark_mgr.h" /* 2026-09-05 阅读器增强：书签管理 */
#include "schedule.h"     /* v1.6：课程表（周计划编排与自动激活） */

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"    /* 词池/阅读器书缓冲：PSRAM 分配 */
#include "nvs_flash.h"

#include <string.h>
#include <stdio.h>
#if INKWORD_GOLDEN_FRAME
#include "selftest_frame.h" /* T2.2 demo env 黄金帧自检（跑完挂起不进 loop） */
#endif

static const char *TAG = "MAIN";
#define FW_VERSION  "1.0.0"
/* 固件版本 getter（快捷菜单设备信息页跨模块取用；FW_VERSION 为文件内宏） */
extern "C" const char *fw_version(void) { return FW_VERSION; }

/* BLE 配网服务默认禁用：Arduino 预编译库未编入 Wi-Fi/BLE coexistence
 * （CONFIG_ESP32_WIFI_SW_COEXIST_ENABLE 编译期固定关闭），Wi-Fi controller
 * 活动时 esp_bt_controller_enable 经 coex_enable() abort（真机崩溃循环）。
 * 迁移到可开 coex 的构建（IDF 框架/自编译 libs）后置 1 启用。 */
#ifndef INKWORD_BLE_PROVISION
#define INKWORD_BLE_PROVISION 0
#endif

/* ---- 词书切换编排（v1.3 T3.1，MENU_DESIGN [学习] 组「词书选择」）：
 * 菜单词书页中键经 menu_ui 调入。顺序：NVS 记录 → 词库重载（预检
 * 失败回退默认）→ LR 按组隔离切换（旧组保存 + 新组恢复，切书不丢
 * 学习进度，LR04）→ 阅读进度键换 scope（同验收口径）→ 模式归位闪卡
 * （临时视图退出由 menu_ui_exit 先行处理；游标归零同模式选择页）。
 * 渲染刷新由调用方 page_router_render_top 承担 ---- */
extern "C" bool deck_flow_switch(int idx)
{
    const deck_info_t *d = deck_manager_at(idx);
    if (!d) return false;

    /* 卡组文件运行期失存（SD 拔出/文件被删）：拒绝切换不落 NVS */
    if (d->file[0] && !storage_file_exists(d->file)) {
        LOG_E("deck file missing: %s", d->file);
        return false;
    }
    if (deck_manager_switch(idx) != 0) return false;

    /* v1.6 课程表：手动切组通知（自动激活路径调 deck_flow_switch 时
     * last_slot>=0 尚未置位，不会误标 manual_override） */
    schedule_on_manual_switch();

    if (load_words_with_catalog() <= 0 ||
        /* 文件型卡组（默认[0]无文件，兑底即正路）必须真正命中 deck
         * 文件；兑底词库非空（>0）但目标未命中（坏 JSON/半落盘）
         * 曾被误判成功：NVS/学习状态污染到坏 deck 键（2026-09-09
         * 修复，判据见 word_loader_last_source） */
        (d->file[0] && word_loader_last_source() != WSRC_DECK)) {
        deck_manager_switch(0);               /* 回退默认链路重装 */
        load_words_with_catalog();
        return false;                   /* 调用方长震反馈 */
    }

    learning_state_reload(word_parser_get_count(), deck_manager_active_id());
    /* 考试冲刺 horizon（v1.5 T5.5）：切组后重设（静态保持，防御性
     * 重注入——exam ymd 不随组变，但时钟可能刚同步到位） */
    learning_state_set_due_horizon(
        exam_urgent() ? exam_days_left() : 0);
    reader_set_progress_scope(deck_manager_active_id());
    study_mode_set(MODE_FLASH);         /* 归位闪卡（study_mode_set 全副作用） */
    return true;
}

/* base 页渲染（T1.4 经 page_router 委托）：有词库走学习页，无词库
 * 走待机页（LAN/配网退出与模式切换后的恢复路径均经此路由；覆盖层
 * 栈非空时 render_top 分发栈顶，不进入本函数） */
static void base_render(void)
{
    study_mode_t m = study_mode_current();
    if (m == MODE_READER)
        /* 阅读模式恢复当前页（无书显示占位页），不回待机页 */
        ui_render_word(m, study_mode_seq_pos());
    else if (m == MODE_CHAT) {
        /* P2B 对话首帧：T2.2 栈化后正常路径经 g_chat_page.enter（代码
         * 迁 chat_mode.c）；此处为防御保留（chat 页意外不在栈时 base
         * 分流仍可画，如未来新模式恢复路径） */
        ui_draw_status(m);
        ui_render_chat(chat_mode_state(), chat_mode_reply());
        epd_gfx_flush();
    }
    else if (m == MODE_BROWSE)
        /* 目录三级视图自绘整屏（标题+列表+提示，刷新策略模块内） */
        browse_mode_render();
    else if (m == MODE_VOICE)
        /* 语音查词四态自绘整屏（三色屏零渲染直接 return） */
        voice_search_render();
    else if (word_parser_get_count() > 0)
        ui_render_word(m, study_mode_current_word_index());
    else
        standby_render_full();
}

/* T1.4 base 页：render=学习/阅读/待机分流；on_button=base_page_on_button
 * （P2 路由补完收编，定义与 g_base_page 注册均后置于该函数）；
 * enter/exit 无（常驻） */

/* 兼容别名 ui_render_current（T1.4 第一步）随 T2.2 栈化退役：
 * quiz_ui 等调用方已全部改 page_router_render_top 直调 */

/* P5 幻影按键吞除武装标志：按键唤醒的会话置位（setup），on_button 吞掉
 * 唤醒后首个中键事件后清位（见 on_button 注释） */
static bool s_wake_swallow_center = false;

/* ---- 用户自定义长按快捷键执行器（2026-09-03，shortcut_map）----
 * 编排镜像 menu_ui act_*（去掉菜单自退；覆盖层进入均经 page_router_push
 * 栈化，enter 失败长震留在当前页）。默认/无动作不由此处理。 */
static void shortcut_exec(sk_action_t act)
{
    switch (act) {
    case SK_ACT_MENU:
        page_router_push(&g_menu_ui_page);
        break;
    case SK_ACT_MODE:   /* =出厂下键长按体 */
        haptic_event(HAPTIC_MODE);
        ui_sfx_play(UI_SFX_MODE);
        study_mode_switch_next();
        page_router_render_top();
        break;
    case SK_ACT_COLLECTION:   /* =菜单 act_collection（进收藏视图） */
        if (learning_state_collected_count() == 0) {
            haptic_event(HAPTIC_ERROR);   /* 空收藏：边界反馈不进入 */
            return;
        }
        haptic_event(HAPTIC_MODE);
        page_router_push(&g_collection_page);   /* 栈串联：enter=置
                                                  * 状态+首帧（计数已
                                                  * 预检非零必成功） */
        break;
    case SK_ACT_WRONGBOOK:   /* =出厂 RST 长按体（非临时视图分支） */
        if (!study_mode_enter_wrongbook()) {
            haptic_event(HAPTIC_ERROR);   /* 无错词：边界反馈 */
            return;
        }
        haptic_event(HAPTIC_MODE);
        page_router_push(&g_wrongbook_page);    /* 状态已置，enter=首帧 */
        break;
#if INKWORD_FEATURE_AI
    case SK_ACT_VOICE:   /* =菜单 act_voice_search */
        if (!study_mode_enter_voice_search()) {
            haptic_event(HAPTIC_ERROR);   /* 无网/未配 Key：边界反馈 */
            return;
        }
        haptic_event(HAPTIC_MODE);
        page_router_push(&g_voice_page);   /* 栈串联：enter=reset+首帧 */
        break;
    case SK_ACT_CHAT:   /* 自由对话直入（菜单 A1 二级页默认项；预检在
                          * enter_chat，失败码仅区分长震不显原因文案） */
    {
        chat_request_t req;
        memset(&req, 0, sizeof(req));
        strlcpy(req.title, "自由对话", sizeof(req.title));
        if (study_mode_enter_chat(&req) != 0) {
            haptic_event(HAPTIC_ERROR);
            page_router_render_top();
            return;
        }
        haptic_event(HAPTIC_MODE);
        page_router_push(&g_chat_page);
        break;
    }
#endif
    case SK_ACT_QUIZ:   /* =菜单 act_quiz */
        if (!study_mode_enter_quiz()) {
            haptic_event(HAPTIC_ERROR);   /* 词库不足：边界反馈 */
            page_router_render_top();
            return;
        }
        haptic_event(HAPTIC_MODE);
        page_router_push(&g_quiz_page);
        break;
    case SK_ACT_BROWSE:   /* =菜单 act_browse */
        if (!study_mode_enter_browse()) {
            haptic_event(HAPTIC_ERROR);   /* 空词库：边界反馈 */
            page_router_render_top();
            return;
        }
        haptic_event(HAPTIC_MODE);
        page_router_push(&g_browse_page);
        page_router_render_top();
        break;
    case SK_ACT_READER:
    case SK_ACT_BOOKSHELF:
        /* 空书架预检（2026-09-07 用户反馈修复）：菜单 act_bookshelf
         * 同款边界——没有书单长震不进入（防空态占位页按键直落单词页） */
        if (book_shelf_scan() == 0) {
            haptic_event(HAPTIC_ERROR);
            return;
        }
        haptic_event(HAPTIC_MODE);
        page_router_push(&g_book_shelf_page);   /* 阅读器增强：进书架选书 */
        break;
    case SK_ACT_GHOST:   /* =出厂上键长按体 */
        refresh_force_full();
        break;
    case SK_ACT_PORTAL:   /* =出厂左键长按体 */
        page_router_push(&g_portal_page);   /* 栈串联：portal 栈化收编 */
        break;
    case SK_ACT_LAN:   /* =出厂右键长按体 */
        page_router_push(&g_lan_page);      /* 栈串联：LAN 接收页栈化收编 */
        break;
    case SK_ACT_COLLECT:   /* =出厂 SET 长按体（星标/收藏切换） */
        if (study_mode_current() == MODE_READER) return;
        haptic_event(HAPTIC_REVIEW);
        learning_state_toggle_collect(study_mode_current_word_index());
        if (study_mode_current() == MODE_COLLECTION &&
            study_mode_after_uncollect()) {
            page_router_render_top();   /* 清空退回闪卡或游标收缩 */
            return;
        }
        page_router_render_top();
        break;
    case SK_ACT_MASTER:   /* =菜单 act_master（墨封/启封切换） */
        if (study_mode_current() == MODE_READER) return;   /* 无当前词 */
    {
        int wi = study_mode_current_word_index();
        if (wi < 0) {
            haptic_event(HAPTIC_ERROR);   /* 空词库/空序列防御 */
            return;
        }
        bool mastered = learning_state_toggle_master(wi);
        haptic_event(HAPTIC_REVIEW);
        if (mastered) ui_stamp_play();    /* 落印仅置位方向（不对称设计） */
        study_mode_after_master();        /* 序列收缩钳位/清空退闪卡 */
        page_router_render_top();
        break;
    }
    case SK_ACT_SETTINGS:
        page_router_push(&g_settings_ui_page);
        break;
    case SK_ACT_SPEAK:
        study_mode_handle_action(3);   /* 发音（含 READER/门控内检） */
        break;
    default:
        break;   /* NONE/DEFAULT 不由本执行器处理 */
    }
}

/* 长按拦截入口（base 页长按 switch 前调用）：true=用户映射命中已执行，
 * false=走出厂长按。守卫——中键菜单锚点恒出厂；错词本/收藏/墨封录
 * 视图内 RST=退出（逃生语义）、SET=移出收藏/启封（序列收缩语义）
 * 不可覆盖 */
static bool shortcut_try_long(nav_key_t id)
{
    if (id == NAV_CENTER) return false;
    study_mode_t m = study_mode_current();
    if (id == NAV_RST && (m == MODE_WRONGBOOK || m == MODE_COLLECTION ||
                          m == MODE_MASTERED))
        return false;
    if (id == NAV_SET && (m == MODE_COLLECTION || m == MODE_MASTERED))
        return false;
    sk_action_t act = shortcut_get(id);
    if (act == SK_ACT_DEFAULT) return false;   /* 键缺失/默认=出厂 */
    shortcut_exec(act);
    return true;
}

/* 词卡视图通用按键路由（栈串联重构 2026-09-08 提取）：base 层
 * （FLASH/听写/拼写）与学习视图栈页（错词本/收藏/墨封录）共用同源
 * ——上/下短按=词内释义翻页→翻词，中=发音，SET=遮蔽/揭晓，RST=进
 * 设置，左/右=自评；出厂长按六键；SET 长按=移出收藏/启封（after_*
 * 清空自动归位时退视图），RST 长按=临时视图退出/进错词本。
 * 返回 false=请求退出视图（栈页 dispatch 统一 pop+render_top；base
 * 调用时 m 恒为持久模式，RST/SET 分支不会返回 false）；
 * word_view_on_button 声明见 word_view_page.h（架构拆分 2026-09-17） */

/* base 页按键编排（P2 路由补完收编 + 栈串联重构 2026-09-08 瘦身）：
 * pron 短事务前置（与栈互斥，base 层触发）→ 待机页转发 → 长按功能
 * （用户自定义 shortcut/阅读模式章节）→ READER/REVIEW 专用路由 →
 * 词卡视图通用路由（word_view_on_button 与临时视图栈页同源）。
 * MODE_VOICE（g_voice_page 栈页）与 LAN/portal（g_lan_page/g_portal_
 * page 栈页）栈化后由栈顶分发，base 分支退役；错词本/收藏/墨封录
 * 临时视图按键由栈页接管。base 为最底层，事件总被消费（true），
 * 忽略分支同样返回 true */
static bool base_page_on_button(nav_key_t id, button_event_t event)
{

    /* P1 跟读评测期间：任意键取消录音 / 关闭结果屏（吞键，pron_task
     * 或 any_key 自恢复词卡；短事务期间不进菜单/翻词） */
    if (study_mode_pron_active() || study_mode_pron_ui_visible()) {
        study_mode_pron_any_key();
        return true;
    }

    /* P2B AI 对话模式：T2.2 栈化后经 g_chat_page 栈顶分发，
     * 退出编排（haptic+exit_chat+pop+render_top）内聚 chat_mode.c */

    /* v1.2 T2.2 快速测验：T2.2 栈化后经 g_quiz_page 栈顶分发，
     * on_button 转发与刷新编排均在 quiz_ui 模块内 */

    /* 教材目录浏览（设计 §A2）经页面路由栈顶分发（T1.4 试点，顶部
     * dispatch；选词 confirm 经 seek 终结视图时 pop_to_base 归位） */

    /* 栈串联重构（2026-09-08）：MODE_VOICE（g_voice_page 栈页）、LAN
     * 接收页/AP portal（g_lan_page/g_portal_page 栈页）均由栈顶分发，
     * 原 base 层转发/任意键退出分支退役 */

    /* 待机页激活时（词库为空），按键交给待机页处理 */
    if (standby_is_active()) {
        standby_on_button(id, event);
        return true;
    }

    /* 长按：用户自定义 shortcut 先行（守卫见 shortcut_try_long）；
     * 阅读模式长按：上/下=章节跳转（阅读器增强 2026-09-05）、
     * RST=退出阅读回闪卡（其余不响应）。出厂六键长按（菜单/清残影/
     * 模式/门户/LAN/收藏/错词本）统一收编 word_view_on_button（与
     * 词卡短按同源单一真相，临时视图栈页共用） */
    if (event == BUTTON_EVENT_LONG_PRESS) {
        if (shortcut_try_long(id)) return true;
        if (study_mode_current() == MODE_READER) {
            if (id == NAV_UP) {
                study_mode_reader_chapter_step(-1);
                return true;
            }
            if (id == NAV_DOWN) {
                study_mode_reader_chapter_step(+1);
                return true;
            }
            if (id == NAV_RST) {
                haptic_event(HAPTIC_MODE);
                study_mode_set(MODE_FLASH);
                page_router_render_top();
                return true;
            }
            return true;   /* 其余长按阅读模式不响应 */
        }
    }

    /* 栈串联重构（2026-09-08）回归修正：旧结构此处滤非短按事件
     * （长按已在上方处理完）；重构后出厂长按六键收编尾部
     * word_view_on_button，本行不得再拦长按（曾致 base 层长按中键
     * 菜单/SET 收藏等全部静默失效，临时视图栈页不受影响） */

    /* 阅读模式短按路由（P3 + 阅读器增强 2026-09-05）：
     * 上/下=翻页，左/右=字号缩放，中=阅读菜单，SET=书签切换，
     * RST=回第一页（“回到当前模式第一条”全局语义） */
    if (study_mode_current() == MODE_READER) {
        switch (id) {
        case NAV_UP:
            study_mode_handle_action(0);      /* 上一页 */
            return true;
        case NAV_DOWN:
            study_mode_handle_action(1);      /* 下一页 */
            return true;
        case NAV_LEFT:
            study_mode_reader_font_step(-1);  /* 字号缩小 */
            return true;
        case NAV_RIGHT:
            study_mode_reader_font_step(+1);  /* 字号放大 */
            return true;
        case NAV_CENTER:
            page_router_push(&g_reader_menu_page);   /* 阅读器菜单 */
            return true;
        case NAV_SET:
            /* SET 短按 = 当前页书签切换（有则删、无则加） */
            {
                int bm_page = study_mode_seq_pos();
                if (bm_page < 0) bm_page = 0;
                if (bookmark_exists(bm_page)) {
                    bookmark_remove(bm_page);
                } else {
                    bookmark_add(bm_page, NULL);
                }
                haptic_event(HAPTIC_REVIEW);
                page_router_render_top();   /* 重绘当前页（书签标记更新） */
            }
            return true;
        case NAV_RST:
            study_mode_reset_cursor();        /* 回第一页 */
            return true;
        default:
            return true;
        }
    }

    /* 复习模式路由（2026-08-24 O3；T2.2 自本函数迁 review_ui.c）：
     * 列表态全接管；详情态仅左/右自评出队回列表，其余放行下方
     * 通用词卡路由 */
    if (study_mode_current() == MODE_REVIEW &&
        review_ui_on_button(id, event))
        return true;

    /* 词卡视图通用路由（栈串联重构收编：原出厂长按 switch + 词卡
     * 短按 switch；base 层 m 恒为 FLASH/听写/拼写等持久模式，
     * 临时视图专属分支由栈页侧消费） */
    return word_view_on_button(study_mode_current(), id, event);
}

/* T1.4 base 页注册（P2 路由补完：on_button 经 dispatch 栈空转发）；
 * enter/exit 无（常驻） */
static const page_t g_base_page = { "base", base_render, base_page_on_button,
                                    NULL, NULL, false };

/* 五向导航按键事件回调（主循环消费）：通用前置（计时刷新/幻影吞除/
 * 确认音）后统一交页面路由——栈顶独占，栈空转发 base_page_on_button */
static void on_button(nav_key_t id, button_event_t event)
{
    /* P5：任何按键事件都刷新无操作计时（含配网/门户转发与退出路径） */
    power_note_activity();

    /* P5：深睡唤醒幻影事件吞除：唤醒键（中键）按住唤醒时，扫描任务
     * 零状态起步，释放时误报 SHORT（中键=发音）或按住超阈值误报 LONG
     * （中键=进配网）；吞掉唤醒后首个中键事件（真实按压最多迟一次
     * 发音，无破坏性；唤醒确认 20ms 震动已在 setup 给出） */
    if (s_wake_swallow_center && id == NAV_CENTER) {
        s_wake_swallow_center = false;
        return;
    }

    /* T1.6 按键按下确认音（滴）：后续语义音（模式/自评/边界）经
     * audio_play_file 打断重播覆盖本音，不会叠播 */
    ui_sfx_play(UI_SFX_KEY);

    /* T1.4 页面路由：覆盖层栈顶独占按键（menu/settings/browse…；
     * 栈空时 dispatch 转发 base_page_on_button，事件必被消费） */
    (void)page_router_dispatch_button(id, event);
}

/* T1.8 开机分阶段计时：esp_timer 自 app 启动累计，串口日志拼出
 * 冷启动→学习页首帧各阶段耗时（NVS→SD→屏→音频按键→Wi-Fi→词库→
 * 首帧），实测回填 PRD §8.1（<3s 目标）；静默心跳会话在 split 处
 * 不返回，打点自然止于前序阶段 */
static int64_t s_boot_last = 0;
static void boot_stamp(const char *stage)
{
    int64_t now = esp_timer_get_time();
    LOG_I("boot: %-11s @%6lld ms (+%lld ms)", stage,
          (long long)(now / 1000),
          (long long)((now - s_boot_last) / 1000));
    s_boot_last = now;
}

/* Arduino setup - 初始化所有组件 */
void setup()
{
    /* 最早期：I2S DOUT (GPIO 6) 内部下拉。NS4150B 功放无 MCU 控制线
     * （R10 上拉常开），启动/烧录期间 I2S 驱动未装载，GPIO 6 悬空被
     * ES8311 DAC 输入拾取→功放→喇叭“滋啦”。内部下拉 ~45kΩ 把悬空
     * 引脚钳 LOW，消除瞬态噪声（2026-09-10 修复） */
    gpio_pulldown_en((gpio_num_t)I2S_DATA_OUT_PIN);

    Serial.begin(115200);
    delay(100);
    boot_stamp("start");

    /* 1. 日志与 NVS */
    log_init();
    LOG_I("=== InkWord firmware %s booting ===", FW_VERSION);

    /* 1.5 P5 电源分流（先于一切外设）：TIMER 唤醒 = 静默心跳会话
     *     （校时/上报/OTA 后回睡，不返回）；中键唤醒/冷启动走下方
     *     正常流程。gpio_hold 跨深睡锁存的 EPD 引脚已在 power_init 释放 */
    if (power_init() == ESP_SLEEP_WAKEUP_TIMER)
        silent_heartbeat_session();
    if (power_woke_by_button())
        s_wake_swallow_center = true;  /* 唤醒键幻影事件吞除武装 */

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    boot_stamp("nvs");

    /* 2. 存储 / 屏幕 / 音频 / 按键 */
    if (storage_init() == 0) {
        storage_list_dir(SD_MOUNT_POINT);
    } else {
        LOG_W("SD card init failed, running without word DB");
    }
    boot_stamp("sd");

    epd_driver_init();
    ui_apply_rotation();                /* NVS 屏幕方向恢复（幂等，首帧前；
                                           默认意图 = 面板默认零动作） */
    epd_clear_screen();                 /* 显示启动白屏 */
    power_mark_periph_online();         /* P5：本会话外设在线（入睡时收口外设） */
    boot_stamp("epd");

    audio_init();    /* ES8311+NS4150B 链路 2026-08-27 验收通过（REG00 正常态
                     + MCLK 实线拓扑）；自检人声改由按键/维护路径触发 */
    /* 音量恢复（2026-08-27）：NVS 镜像同步进 es8311 驱动状态（默认 75=
     * 0xBF 历史听感；此后 dac_start 起播回写，设置页/菜单即时调节） */
    es8311_set_volume(settings_volume());
    /* 粗细恢复（2026-08-27 P2）：NVS 同步进 epd 表选择（默认关=常规表；
     * 设置页切换即时 apply，见 settings_ui case 5） */
    epd_gfx_set_bold(settings_bold_enabled());
    ui_sfx_init();                   /* T1.6 提示音样本探测（缺样本静默降级） */
    haptic_init();                   /* 触觉反馈（P2 震动）：先于按键扫描任务 */
    max17048_init();                 /* T2.6 电量计（共享 I2C，不在位静默降级） */
    if (power_woke_by_button())
        haptic_event(HAPTIC_KEYPRESS); /* P5：唤醒确认 20ms（先于屏恢复完成） */
    button_handler_init();
    /* T0.2（修 C2）：事件流改队列——扫描任务只入队，on_button 由
     * loop 经 button_wait 在主任务上下文消费（原回调直调模式下，
     * on_button 在扫描任务里执行渲染+刷新（0.4~14.6s），期间扫描
     * 停摆、连按丢失；队列化后刷新阻塞期间事件排队不丢） */
    boot_stamp("keys");

    /* 2.5 T1.4 页面路由：注册 base 页（先于一切 push/渲染；覆盖层
     *     栈 menu/settings/browse 见各模块 g_*_page） */
    page_router_init(&g_base_page);

    /* 3. 刷新调度器：T1.7 三页面保养阈值统一公式 desc×factor
     * （学习 1.0/待机 1.5/配网 1.25，系数入 layout_profile）：
     * 2026-08-26 wft0290 调优改：原硬编码 8 与 desc 脱钩；残影为
     * 单相快刷固有特性，wft0290 取 4 加频清除，全刷 3.4s 洗净实测；
     * 416 屏 8×100/100=8，行为零变化 */
    const epd_panel_desc_t *pd = epd_panel_desc();
    int learn_base = (pd && pd->partial_count_full_refresh > 0)
                   ? pd->partial_count_full_refresh : 8;
    refresh_scheduler_init(learn_base * layout_profile_get()->partial_std / 100);

    /* 4. WiFi 联网（失败不阻塞主流程）；同步凭据（base URL / 设备 key）
     *    从 NVS 恢复到 sync_client，首次注册留待联网后 background_task */
#if INKWORD_FEATURE_WIFI
    wifi_manager_init();
#endif
#if INKWORD_FEATURE_CLOUD
    sync_credentials_load();
#endif

    /* 4.5 Wi-Fi 配置：无凭据时自动开启 AP 配网门户
     *     （手机连 InkWord-Setup 热点后自动弹出配置页）；
     *     软键盘配置 UI 仍可长按 C 进入。栈串联重构（2026-09-08）：
     *     portal 栈化收编（g_portal_page），任意键退出/配网成功自动
     *     收尾均回待机页（原直调 lan_portal_enter 不入栈旁路退役） */
#if INKWORD_FEATURE_WIFI
    wifi_config_ui_init();
#endif
#if INKWORD_FEATURE_WIFI && INKWORD_FEATURE_LAN
    if (!wifi_has_saved_credentials()) {
        LOG_W("no saved WiFi, starting AP portal");
        page_router_push(&g_portal_page);
    }
#endif

    /* 4.6 BLE 配网服务（App 扫描发现/配网；失败仅告警，
     *     Portal 与软键盘配网路径不受影响）。
     *     默认禁用，根因见文件头 INKWORD_BLE_PROVISION 注释 */
#if INKWORD_BLE_PROVISION && INKWORD_FEATURE_WIFI
    ble_provision_init();
#endif

    /* 5. 标记当前固件有效，防止 OTA 回滚 */
    ota_mark_valid();
    boot_stamp("wifi");

    /* 6. 加载词库：词池 PSRAM 分配 + 卡组扫描 + 三级递降装载 +
     *    演示词兑底（T1.3 迁 word_loader.c，行为零变化） */
    word_loader_init();

    /* 6.5 本地学习状态（P1 错词本/收藏）：按卡组+词库规模锁定并从
     *     该组 NVS 键恢复（LR04）；必须先于 study_mode_init/首次渲染
     *     （错词序列与收藏标记依赖） */
    learning_state_init(word_parser_get_count(), deck_manager_active_id());
    /* v1.6 课程表：装载周配置（NVS 无键则默认关闭，开箱不变） */
    schedule_init();
    /* 考试冲刺 horizon（v1.5 T5.5）：urgent（≤7 天）时到期视图放宽
     * 到考前将到期全部入队（日期反推优先清账；时钟后同步时切组/
     * 下次启动生效——冷启动自治钟未同步则保持 0，同步后切组或重启注入） */
    learning_state_set_due_horizon(
        exam_urgent() ? exam_days_left() : 0);
    boot_stamp("words");

    /* 6.9 P5 深睡唤醒时钟恢复：自治钟基准对经 RTC 慢钟差分重建
     *     （仅按键唤醒路径；冷启动/复位 cause=UNDEFINED 不走此路，
     *     维持未同步留白等 HTTP 校准。须在待机页首渲染/tick 之前） */
    if (power_woke_by_button())
        standby_time_restore();

    /* 7. 初始化待机页（恢复 NVS 天气缓存），进入上次学习模式；
     *    无词库时渲染待机页（时钟/日历/天气） */
    standby_init();
    /* T1.2：测验单词字号适配注入（ui_fit_font/ui_word_start_size
     * 为 static 工具，经函数指针传入避免 quiz_ui 反向依赖） */
    quiz_ui_set_font_fit(ui_fit_font, ui_word_start_size);
    /* 7.5 阅读引擎（P3）：找书整本入 PSRAM + 建页表；必须先于
     *     study_mode_init（READER 模式恢复阅读页进度/页数依赖页表）。
     *     v1.3 T3.1：进度键 scope 先注入（非默认卡组 rd_* 加 id 后缀，
     *     font_level_restore 在 init 内即消费 scope 键） */
    reader_set_progress_scope(deck_manager_active_id());
    reader_engine_init();
    study_mode_init();
    if (word_parser_get_count() > 0 && study_mode_current() != MODE_READER) {
        study_mode_handle_action(1);    /* 渲染第一条 */
    } else {
        page_router_render_top();      /* READER 书页/占位页 或 待机页 */
    }
    boot_stamp("first-frame");

    /* v1.6 课程表：不再启动时强制显示（恢复原有启动流程，直接进入学习模式）。
     * 课程表可通过菜单进入（菜单 → 课程表设置）。 */

    /* 8. 启动后台任务（心跳/LAN/天气/OTA；sync_session 内聚任务细节） */
    sync_background_task_start();

    LOG_I("=== InkWord ready ===");
    boot_stamp("ready");

#if INKWORD_SCREEN_AGING_TEST
    screen_aging_test_enter();
#endif

#if INKWORD_GOLDEN_FRAME
    /* T2.2：demo env 自检序列（首帧已绘、词库/字体/后台任务就绪后；
     * 跑完内部挂起，不进 loop） */
    selftest_frame_run();
#endif

#if INKWORD_SCREEN_AGING_TEST
    screen_aging_test_enter();
#endif
}

/* Arduino loop - 主循环（T0.2/T0.3 事件驱动核心：按键队列消费 +
 * LAN 直传帧投递 + 分钟级待机 tick；非待机状态时零开销返回；学习
 * 状态脏标记静默 5s 后在非按键路径落盘。100ms 队列超时即循环节拍，
 * 原 vTaskDelay(1000) 退役；刷新/同步等重活在按键处理内阻塞本
 * 循环期间，待机 tick 暂停（分钟级时钟更新无感），事件与帧在队列/
 * 双缓冲中排队待处理 */
void loop()
{
    nav_key_t     id;
    button_event_t ev;
    if (button_wait(&id, &ev, 100)) {
#if INKWORD_SCREEN_AGING_TEST
        if (screen_aging_test_is_active()) {
            screen_aging_test_on_button((int)id, (int)ev);
        } else
#endif
            on_button(id, ev);
    }
#if INKWORD_FEATURE_LAN
    /* T0.3（修 C1 终态）：LAN 帧由主任务直刷（httpd 只收帧置就绪），
     * EPD 回归单写者；无待刷帧时零开销返回 */
    lan_display_drain_frame();
#endif
#if INKWORD_FEATURE_WIFI
    /* T2.2 wifi 页栈回收：exit_config 在 wifi 任务上下文置退出，
     * 页栈操作统一回主循环（先查 s_active 防误弹正常驻留页；强制
     * 全刷回词卡——wifi 期间任意帧含白屏过渡，全刷洗状态栏；
     * 正常路径 exit_config 已先行渲染，本轮仅补位） */
    if (!wifi_config_ui_is_active() &&
        page_router_pop_if(&g_wifi_ui_page)) {
        ui_force_full_refresh_next();
        page_router_render_top();
    }
#endif
#if INKWORD_FEATURE_WIFI && INKWORD_FEATURE_LAN
    /* 栈串联重构：portal 配网成功自动收尾回收（portal_monitor_task
     * 任务上下文置 s_portal_auto_exit 标志，页栈单写者纪律回主循环
     * pop+render_top；portal 已被用户按键退出时 pop_if NULL 零动作，
     * wifi 页回收同款模式） */
    if (lan_portal_take_auto_exit() &&
        page_router_pop_if(&g_portal_page))
        page_router_render_top();
#endif
    standby_tick();
    learning_state_maybe_save();  /* LR02 sparse 延迟保存（无脏零开销） */
    power_maybe_sleep();          /* P5：无操作超时且无禁睡条件则入睡（不返回） */
}
