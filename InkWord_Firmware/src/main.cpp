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
 *        配网/门户/LAN/设备信息；Wi-Fi 配网降为菜单项，2026-08-23）；
 *   左 短按=自评「忘记」Q1（SM-2 质量分 1：连错+1，>0 入错词本）/ 长按=进入 AP 直连/配网门户
 *        （手机连 InkWord-Setup 热点直传，绕开路由器隔离；任意键退出）；
 *   右 短按=自评「简单」Q5（SM-2 质量分 5：连错清零，错词本中移出）/ 长按=进入 LAN 接收页（同网浏览器直传，任意键退出）；
 *   SET 短按=遮蔽/揭晓释义（闪卡自测；待机页=轮换下一条引文）/ 长按=收藏/取消当前词（左栏 * 标记；收藏视图内=移出序列）；
 *   RST 短按=回到当前模式第一条 / 长按=临时视图进出（错词本或收藏浏览，
 *        按当前所在视图退出，否则进错词本）。
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
#include "epd_driver.h"
#include "audio_player.h"
#include "es8311.h"        /* 2026-08-27 音量恢复：启动后 NVS 镜像同步进 codec 驱动状态 */
#include "button_handler.h"
#include "haptic.h"
#include "ui_sfx.h"     /* T1.6 提示音：按键确认/自评/模式/边界 */
#include "quiz_session.h" /* v1.2 T2.2：四选一出题核心（纯 C，泛化回调） */
#include "storage_manager.h"
#include "refresh_scheduler.h"
#include "word_parser.h"
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
#include "catalog_index.h" /* 教材目录索引（browse 数据源，词库装载尾部构建） */
#include "browse_mode.h"   /* 教材目录浏览（MODE_BROWSE 三级目录临时视图） */
#include "voice_search.h"  /* AI 语音查词（MODE_VOICE 四态临时视图） */
#include "wifi_manager.h"
#include "wifi_config_ui.h"
#include "menu_ui.h"      /* 快捷菜单（功能菜单，长按中进入） */
#include "sync_client.h"
#include "ota_manager.h"
#include "lan_display_server.h"
#include "standby_page.h"
#include "reader_engine.h"   /* 阅读模式（P3）：书分页/字号/进度 */
#include "ble_provision.h"
#include "power_manager.h"   /* P5 深睡/唤醒分流与入睡检查 */

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"    /* 词池/阅读器书缓冲：PSRAM 分配 */
#include "esp_mac.h"          /* esp_read_mac：首次注册的设备身份 */
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "MAIN";
#define FW_VERSION  "1.0.0"
/* 固件版本 getter（快捷菜单设备信息页跨模块取用；FW_VERSION 为文件内宏） */
extern "C" const char *fw_version(void) { return FW_VERSION; }
/* 词库容量（PRD §7.2 容量红线 2026-08-20 解除）：词池迁 PSRAM 后
 * 上限 4000 词（词库扩展四字段后 sizeof(WordEntry)≈1096B，
 * 4000 词 ≈ 4.2MB；与阅读器单书上限 4MB 并发最坏 ≈ 8.2MB——仅
 * “满词库+4MB 大书”同时存在时才触顶，实际书多在 1-2MB 且词池
 * 分配失败时逐半降级兼容）。实际分配不足时 setup 内逐级降级，
 * 见 s_word_pool 分配处。
 * v1.4 T4.2 重估：口径 = 单活跃卡组（load_active_words 只装当前
 * deck，4000 上限即单 deck 词条上限；多卡组并存只多占 SD，不多占
 * PSRAM——词池/解析 DOM/学习状态数组均随活跃组重载复用） */
#define MAX_WORDS   4000

/* 后端 API Base URL（P2 上报闭环）：部署时 -D INKWORD_API_BASE=... 覆盖，
 * 或经 NVS "inkword"/"api_url" 覆盖（配网 UI 扩展后可写）；
 * http: 前缀自动走明文 TCP（本地开发后端，见 sync_client fill_cfg） */
#ifndef INKWORD_API_BASE
#define INKWORD_API_BASE "https://api.einkword.com"
#endif

/* 演示词库开关：inkword-s3-demo 环境置 1；无 SD 词库时加载内嵌 5 词，
 * 用于学习页按键（翻词/SET 遮蔽/RST 回首）的整机验证；
 * 正式构建保持 0，无词库仍走待机页（用户定稿行为） */
#ifndef INKWORD_DEMO_WORDS
#define INKWORD_DEMO_WORDS 0
#endif

/* BLE 配网服务默认禁用：Arduino 预编译库未编入 Wi-Fi/BLE coexistence
 * （CONFIG_ESP32_WIFI_SW_COEXIST_ENABLE 编译期固定关闭），Wi-Fi controller
 * 活动时 esp_bt_controller_enable 经 coex_enable() abort（真机崩溃循环）。
 * 迁移到可开 coex 的构建（IDF 框架/自编译 libs）后置 1 启用。 */
#ifndef INKWORD_BLE_PROVISION
#define INKWORD_BLE_PROVISION 0
#endif

/* 词池：PSRAM 堆分配（原 DRAM 静态数组仅容 64 词；与 reader_engine
 * 书缓冲同策略 MALLOC_CAP_SPIRAM，setup 内 storage_init 后分配） */
static WordEntry *s_word_pool = NULL;
static int        s_word_cap = 0;   /* 实际分配容量（降级后 < MAX_WORDS） */

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
#define UI_STATUS_H     (UI_TINY ? 24 : 32)  /* 状态栏高度（内容区顶 y；无窗口差分下不再要求 8 对齐） */
#define UI_MARGIN_X     (UI_TINY ? 8 : 16)   /* 左右留白 */
#define UI_STATUS_BASE  (UI_STATUS_H - 10)             /* 状态栏文字基线（22/14） */
/* ---- 学习页单列版式（全档位统一，2026-08-23 重设计）----
 * 上下结构：头部单词（全宽大字自适应）+ 音标 + 收藏星标；正文流 =
 * 释义+词根全宽分页；底部标签行全宽。双栏版式退役（真机反馈 136px
 * 右栏 7 字/行阅读体验差，416x240 全宽 21 字/行提升 3 倍）；
 * 字号档位派生：TINY/SMALL 16px / MID+ 20px */
#define UI_MEAN_LEVEL   (layout_profile_get()->kind <= LAYOUT_SMALL \
                         ? (settings_font_mode() >= 1 ? 1 : 0) \
                         : (settings_font_mode() >= 1 ? 2 : 1))  /* 正文字号级：
 * 档位默认 TINY/SMALL 16px / MID+ 20px；大字/特大档（set_font>=1）
 * 整体 +1 级（20/24px），行距与几何全部由本宏派生自适应。2026-08-27
 * P1a 三档化：意图相对档位表达（set_rot 同哲学）——TINY 屏宽 122~
 * 128px 下 24px 每行仅 3~4 字 / SMALL 横屏 176 高正文行数趋零，
 * 特大档(2)在 TINY/SMALL 钳位至 20px（渲染等价大字，语义不漂移） */
#define UI_WORD_BASE    (UI_STATUS_H + (UI_TINY ? 24 \
                         : (UI_MEAN_LEVEL ? 36 : 32)))  /* 单词基线（68/64/48） */
#define UI_PHON_TOP     (UI_WORD_BASE + (UI_TINY ? 6 : 9))     /* 音标行 16px 点阵顶（77/73/54） */
#define UI_BODY_TOP     (UI_PHON_TOP + 16 + (UI_TINY ? 4 : 11)) /* 正文流首行顶（104/100/74） */
#define UI_BODY_LH      (UI_MEAN_LEVEL ? 24 : 20)  /* 正文行距：字级 +4（reader 惯例） */
/* 行数按屏高派生：底部预留 30 = 标签行 + 余量（末行文字底与标签顶
 * 错开，MID 末行底 196 < 标签顶 206）；416x240=4 行、400x300=6、
 * 264x176=2、122x250 竖屏=7、128x296 竖屏=9（TINY 预留收至 26） */
#define UI_BODY_RESERVE (UI_TINY ? 26 : 30)
#define UI_BODY_LINES_  ((epd_gfx_height() - UI_BODY_RESERVE - UI_BODY_TOP) / UI_BODY_LH)
#define UI_BODY_LINES   (UI_BODY_LINES_ < 1 ? 1 : UI_BODY_LINES_)  /* 下限 1：
 * 极端几何（窄屏高字号叠加）防御，正文区至少 1 行可翻页（P1a） */
#define UI_BODY_MAX_W   (epd_gfx_width() - 2 * UI_MARGIN_X)   /* 全宽正文（392/232/106/112） */
#define UI_FOOT_BASE    (epd_gfx_height() - 16)        /* 底部标签基线：底边距 16（224） */
#define UI_FOOT_TOP     (UI_FOOT_BASE - 18)             /* 中文 tag 16px 点阵顶：基线上 16+2（206） */
/* ---- v1.4 T4.3 poem-card 头部几何：诗行（正文字号大一级，绝句两句
 * 内）+ 拼音行（16px），译文区从拼音行下 8px 起（ui_mean_geom 派生）；
 * MID 默认档 124 起可容 5 行译文，SMALL 1 行/页分页翻 ---- */
#define UI_POEM_LEVEL     (UI_MEAN_LEVEL < 2 ? UI_MEAN_LEVEL + 1 : 2)
#define UI_POEM_LH        (UI_POEM_LEVEL * 4 + 20)     /* 22/26/30：字级+6 呼吸感 */
#define UI_POEM_LINES     2                            /* 诗行上限（超行截断，T4.4 约定存精华句） */
#define UI_POEM_TOP       (UI_STATUS_H + 8)
#define UI_POEM_PIN_TOP   (UI_POEM_TOP + UI_POEM_LINES * UI_POEM_LH + 4)
#define UI_POEM_TRANS_TOP (UI_POEM_PIN_TOP + 16 + 8)   /* 拼音行底 + 8 */

/* ---- 复习模式词表视图（2026-08-24，PRD 5.2「紧凑显示+SRS 到期词」落地；
 * 百词斩复习范式借鉴）：到期词紧凑两列词表 + 中键进词卡详情，
 * 左/右自评即出队（游标钳位），列表态/详情态两态由渲染层承载 ---- */
#define RV_ITEM_H   (UI_TINY ? 28 : (layout_profile_get()->kind == LAYOUT_SMALL \
                                    ? 36 : 44))    /* 对齐 menu_ui 列表行高 */
/* 测验纵列选项区顶（题干 1/3 内容区）；行高在 ui_draw_quiz_option
 * 内由可用区四等分与 RV_ITEM_H 取小（2026-08-25 真机反馈收窄：
 * QUIZ_DESIGN §5「一屏四行」在 MID 416x240 沿用 RV_ITEM_H=44 实测
 * 违约——opt_top 93 + 4×44 = 269 出屏（提示栏 206），D 项不可见且
 * 测验选项恒 4 项无滚动语义；收窄后 MID 416x240 行高 27、400x300=37，
 * TINY 竖屏充裕仍 28，SMALL 恒网格不进纵列） */
#define QZ_OPT_TOP  (UI_STATUS_H + (epd_gfx_height() - UI_STATUS_H \
                                    - RV_HINT_H) / 3)
#define RV_LIST_TOP (UI_STATUS_H + 4)
#define RV_HINT_H   (UI_TINY ? 18 : 24)            /* 底部提示行预留 */
#define RV_VISIBLE  ((epd_gfx_height() - UI_STATUS_H - RV_HINT_H - 4) / RV_ITEM_H)
#define RV_SB_W     4                               /* 滚动条宽（menu_ui 同款） */
static bool s_review_detail = false;   /* false=词表 / true=词卡详情 */
static int  s_rv_off = 0;              /* 词表滚动窗口偏移 */

/* 快速测验编排块（下方）先于渲染区引用以下符号，声明/定义前置
 * （C++ 静态变量单次定义：s_last_mode 自原渲染区上移至此） */
static study_mode_t s_last_mode = MODE_COUNT; /* 无效值：首帧强制全刷 */
static int ui_fit_font(const char *text, int start_size, int max_w);
static int ui_word_start_size(void);   /* P1b：单词字号偏好→fit 起步档 */
extern "C" void ui_render_word(study_mode_t mode, int index);
extern "C" void ui_render_current(void);

/* ---- 快速测验视图（v1.2 T2.2，QUIZ_DESIGN P1-b）：题池重映射 + 渲染 +
 * 作答编排留 main.cpp（ui_draw_review_list 先例）；出题核心 quiz_session
 * 纯 C（T2.1，native-test 验证）。核心域词条索引 [0,n) 经 s_quiz_pool
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
     * T3 让出中键作答后重播迁长按中（quiz_on_button） */
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
    for (int i = 0; i < total && n < QUIZ_POOL_MAX; i++)   /* 2) 新词补足 */
        if (learning_state_is_new(i)) s_quiz_pool[n++] = i;
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

/* 进入测验会话（study_mode_enter_quiz 成功后由菜单 act_quiz 执行）：
 * 题池 → 核心 start → 首帧（模式切换自然全刷）；menu_ui（C）
 * 经 extern 声明调用（ui_render_current 同款先例） */
extern "C" void quiz_flow_start(void)
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
        ui_render_current();
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
static void ui_draw_quiz(void)
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
                              ui_fit_font(w->text, ui_word_start_size(),
                                          UI_BODY_MAX_W));
        cjk_text_draw_wrap(UI_MARGIN_X, opt_top - 20, UI_BODY_MAX_W,
                           0, 18, 1, quiz_txt(q.opt_word[1], 1),
                           EPD_GFX_BLACK);
    } else if (w) {
        /* T1：单词大字（全宽自适应降字号）+ 音标（斜杠包裹惯例） */
        int stem_base = UI_STATUS_H + (opt_top - UI_STATUS_H) * 2 / 3;
        epd_gfx_draw_text(UI_MARGIN_X, stem_base, w->text, EPD_GFX_BLACK,
                          ui_fit_font(w->text, ui_word_start_size(),
                                      UI_BODY_MAX_W));
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
        s_last_mode = MODE_COUNT;   /* 强制小结页全刷（低频帧） */
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
static void quiz_on_button(nav_key_t id, button_event_t event)
{
    if (s_quiz_summary) {              /* 小结页：任意键退出 */
        study_mode_exit_quiz();
        ui_render_current();
        return;
    }

    if (event == BUTTON_EVENT_LONG_PRESS) {
        if (id == NAV_RST) {           /* RST 长/短按均退出（临时视图语义） */
            study_mode_exit_quiz();
            ui_render_current();
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
                ui_render_current();
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
            ui_render_current();
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
        ui_render_current();
        return;
    default:
        return;
    }
}

/* 活跃词库装载（v1.3 T3.1，setup 与 deck_flow_switch 共用链路）：
 * 活跃卡组文件 → SD words.json → 内嵌兜底 三级递降；任一级成功
 * 即返回（内嵌常在 rodata，末级必达）。返回词条数（<0 全失败）。 */
static int load_active_words(void)
{
    /* v1.4 T4.5 字库子集级联：随活跃卡组装载/切换同步装载该组 SD 子集
     * 字库（/sdcard/fonts/deck_<id>.bin，无文件=主集已覆盖的正常路径，
     * 静默通过；旧组子集在 load 内部先退场）。置于词条链路之前：
     * 子集只依赖 deck_active，与哪级词条源命中无关 */
    cjk_font_sd_load(deck_manager_active_id());

    const char *deck_file = deck_manager_active_file();
    if (deck_file) {
        int n = word_parser_load(deck_file, s_word_pool, s_word_cap);
        if (n > 0) {
            LOG_I("deck DB loaded: %d entries (%s)", n, deck_file);
            return n;
        }
        LOG_E("deck load failed: %s, fallback", deck_file);
    }

    const char *word_file = SD_MOUNT_POINT "/words.json";
    if (storage_file_exists(word_file)) {
        int n = word_parser_load(word_file, s_word_pool, s_word_cap);
        if (n > 0) {
            LOG_I("word DB loaded: %d entries", n);
            return n;
        }
    }

    /* 出厂内嵌兜底（2026-08-23）：无 SD 卡开箱即用。词库随固件烧入
     * rodata（platformio.ini embed_files，生成链见
     * tools/default_vocab），load_mem 零拷贝直吃。SD/卡组词库存在时
     * 优先（可更新、可携带 cloudId）；内嵌版本无 cloudId（本地词条，
     * 评分/收藏不上报），在线同步/导出路径下发的词库才携带 */
    extern const uint8_t _binary_src_default_words_json_start[];
    extern const uint8_t _binary_src_default_words_json_end[];
    return word_parser_load_mem(
        (const char *)_binary_src_default_words_json_start,
        (size_t)(_binary_src_default_words_json_end -
                 _binary_src_default_words_json_start),
        s_word_pool, s_word_cap);
}

/* 词库装载 + 目录索引（设计 §A1）：装载链路统一挂载点，setup 与
 * deck_flow_switch 共用（词库切换时 catalog_build 内部 free 重建） */
static int load_words_with_catalog(void)
{
    int n = load_active_words();
    catalog_build();
    return n;
}

/* ---- 词书切换编排（v1.3 T3.1，MENU_DESIGN [学习] 组「词书选择」）：
 * 菜单词书页中键经 menu_ui 调入。顺序：NVS 记录 → 词库重载（预检
 * 失败回退默认）→ LR 按组隔离切换（旧组保存 + 新组恢复，切书不丢
 * 学习进度，LR04）→ 阅读进度键换 scope（同验收口径）→ 模式归位闪卡
 * （临时视图退出由 menu_ui_exit 先行处理；游标归零同模式选择页）。
 * 渲染刷新由调用方 ui_render_current 承担 ---- */
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

    if (load_words_with_catalog() <= 0) {  /* 重载失败回退默认链路重装 */
        deck_manager_switch(0);
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
 * start 4，视觉零变化（2026-08-27 P1b）。前置声明见编排块前 */
static int ui_word_start_size(void)
{
    int w = settings_word_size();
    return w == 2 ? 2 : (w == 1 ? 3 : 4);
}

/* 字号自适应：从 start_size 逐级降到能放进 max_w 的字号 */
static int ui_fit_font(const char *text, int start_size, int max_w)
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
extern "C" void ui_render_word(study_mode_t mode, int index);  /* 下方定义 */
extern "C" void ui_render_current(void);  /* 下方定义（menu_ui 恢复退出用） */
extern "C" void ui_render_chat(chat_state_t st, const char *text);  /* 下方定义（ui_render_current 首帧分流） */

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

/* 页数 = 总行数向上取整 / 每页行数（空文 1 页兕底） */
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
static bool ui_mean_page_step(int dir)
{
    if (study_mode_current() == MODE_READER) return false;
    if (!study_mode_is_revealed()) return false;   /* 遮蔽自测态无页可翻 */

    const WordEntry *w = word_parser_get(study_mode_current_word_index());
    if (!w) return false;

    int np = s_mean_page + dir;
    if (np < 0 || np >= ui_mean_total_pages(w)) return false;
    s_mean_page = np;
    ui_render_word(study_mode_current(), study_mode_current_word_index());
    return true;
}

/* 绘制状态栏：模式名（左）+ 序号（右，错词本=序号/错词数）+ 分隔线 */
static void ui_draw_status(study_mode_t mode)
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
                               /*level*/0, 0, 1, foot, EPD_GFX_BLACK);
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

    /* 收藏星标：无头部行，画在正文区右上角（词卡音标行右缘同语义） */
    if (learning_state_is_collected(study_mode_current_word_index())) {
        int sw = cjk_text_width(0, "*");
        cjk_text_draw(epd_gfx_width() - UI_MARGIN_X - sw, UI_STATUS_H + 6,
                      0, "*", EPD_GFX_BLACK);
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
    /* 收藏星标：拼音行右缘（遮蔽/默写态照画，word 版听写先例） */
    if (learning_state_is_collected(study_mode_current_word_index())) {
        int sw = cjk_text_width(0, "*");
        cjk_text_draw(epd_gfx_width() - UI_MARGIN_X - sw, UI_POEM_PIN_TOP,
                      0, "*", EPD_GFX_BLACK);
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
    } else {
        epd_gfx_draw_text(UI_MARGIN_X, UI_WORD_BASE, w->text, EPD_GFX_BLACK,
                          ui_fit_font(w->text, ui_word_start_size(),
                                      UI_BODY_MAX_W));

        if (w->phonetic[0]) {
            /* 词典惯例斜杠包裹：裸 IPA 补 / /，自带包裹符（/[）不双包 */
            if (w->phonetic[0] == '/' || w->phonetic[0] == '[')
                cjk_text_draw(UI_MARGIN_X, UI_PHON_TOP, 0,
                              w->phonetic, EPD_GFX_BLACK);
            else {
                char ph[WORD_PHONETIC_MAX + 4];
                snprintf(ph, sizeof(ph), "/%s/", w->phonetic);
                cjk_text_draw(UI_MARGIN_X, UI_PHON_TOP, 0,
                              ph, EPD_GFX_BLACK);
            }
        }
    }

    /* 收藏标记（P1）：已收藏词在音标行右缘显示 *（SET 长按切换；
     * 点阵 ASCII 与音标行同 16px 级，2026-08-23 随音标行点阵化统一）；
     * 听写遮蔽态照画（无拼写信息量） */
    if (learning_state_is_collected(study_mode_current_word_index())) {
        int sw = cjk_text_width(0, "*");
        cjk_text_draw(epd_gfx_width() - UI_MARGIN_X - sw, UI_PHON_TOP,
                      0, "*", EPD_GFX_BLACK);
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

/* 复习到期词表（列表态）：两列紧凑行（左词 FreeSans / 右释义首行
 * 截断点阵）+ 反选高亮 + 滚动条 + 底部提示（menu_ui 列表范式）；
 * 行取词直接经 learning_state_due_at（REVIEW 序列=due 视图） */
static void ui_draw_review_list(void)
{
    int total = study_mode_seq_total();
    int sel   = study_mode_seq_pos();

    /* 滚动窗口跟随 */
    if (sel < s_rv_off) s_rv_off = sel;
    if (sel >= s_rv_off + RV_VISIBLE) s_rv_off = sel - RV_VISIBLE + 1;
    int max_off = total > RV_VISIBLE ? total - RV_VISIBLE : 0;
    if (s_rv_off > max_off) s_rv_off = max_off;
    if (s_rv_off < 0) s_rv_off = 0;

    epd_gfx_fill_rect(0, UI_STATUS_H, epd_gfx_width(),
                      epd_gfx_height() - UI_STATUS_H, EPD_GFX_WHITE);

    int body_w = epd_gfx_width() - 2 * UI_MARGIN_X - RV_SB_W - 4;
    for (int i = 0; i < RV_VISIBLE; i++) {
        int idx = s_rv_off + i;
        if (idx >= total) break;
        int wi = learning_state_due_at(idx);
        const WordEntry *w = wi >= 0 ? word_parser_get(wi) : NULL;
        if (!w) break;

        int y = RV_LIST_TOP + i * RV_ITEM_H;
        bool s = (idx == sel);
        if (s)
            epd_gfx_fill_rect(UI_MARGIN_X, y, body_w, RV_ITEM_H - 4,
                              EPD_GFX_BLACK);

        /* 左：词（FreeSans size 2；半宽 ASCII 与点阵释义行视觉平衡） */
        int tw, th;
        epd_gfx_text_bounds(w->text, 2, &tw, &th);
        epd_gfx_draw_text(UI_MARGIN_X + 4, y + RV_ITEM_H * 3 / 4,
                          w->text, s ? EPD_GFX_WHITE : EPD_GFX_BLACK, 2);

        /* 右：释义首行截断（16px 点阵单行；剩宽 <32px 跳过） */
        int mx = UI_MARGIN_X + 4 + tw + 12;
        int mw = UI_MARGIN_X + body_w - 6 - mx;
        if (mw >= 32 && w->meaning[0])
            cjk_text_draw_wrap(mx, y + (RV_ITEM_H - 16) / 2, mw, 0, 0, 1,
                               w->meaning, s ? EPD_GFX_WHITE : EPD_GFX_BLACK);
    }

    /* 滚动条（menu_ui 同款滑块） */
    if (total > RV_VISIBLE) {
        int x = epd_gfx_width() - UI_MARGIN_X;
        int h = RV_VISIBLE * RV_ITEM_H;
        epd_gfx_draw_rect(x, RV_LIST_TOP, RV_SB_W, h, EPD_GFX_BLACK);
        int thumb_h = h * RV_VISIBLE / total;
        if (thumb_h < RV_SB_W * 2) thumb_h = RV_SB_W * 2;
        epd_gfx_fill_rect(x, RV_LIST_TOP + (h - thumb_h) * s_rv_off /
                          (total - RV_VISIBLE), RV_SB_W, thumb_h, EPD_GFX_BLACK);
    }

    /* 底部提示行（同学习页 foot 位；RST 直达设置 2026-08-27） */
    cjk_text_draw(UI_MARGIN_X, UI_FOOT_TOP, 0,
                  "中 详情 · 左/右 自评出队 · RST 设置", EPD_GFX_BLACK);
}

/* LAN 直传外部内容整帧直刷后调用：GFX previous 缓冲已失配，
 * 置 s_last_mode 无效值强制下一次学习界面渲染走全刷 */
extern "C" void ui_force_full_refresh_next(void)
{
    s_last_mode = MODE_COUNT;
}

/* v1.2 T2.5：设置字号档变更后的排版失效（settings_ui 退出时调用）——
 * UI_MEAN_LEVEL 派生几何变化须全刷重排，释义分页游标与绑定词一并归零 */
extern "C" void ui_force_font_refresh(void)
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
extern "C" void ui_apply_rotation(void)
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
extern "C" void ui_render_word(study_mode_t mode, int index)
{
    if (wifi_config_ui_is_active()) return; /* 配置页期间不绘制学习页 */
    if (lan_server_is_active()) return;     /* LAN 接收页期间不绘制学习页 */
    if (menu_ui_is_active()) return;        /* 快捷菜单期间不绘制学习页 */
    if (settings_ui_is_active()) return;    /* 设置页期间不绘制（T2.5） */
    if (study_mode_pron_active() ||
        study_mode_pron_ui_visible()) return; /* P1 跟读三态屏独占内容区 */

    /* 模式切换重置复习列表态（详情态只在会话内保持） */
    if (mode != s_last_mode) s_review_detail = false;

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
    if (mode == MODE_REVIEW && !s_review_detail) {
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
        ui_draw_review_list();

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

        ui_draw_quiz();
        if (need_full)
            epd_gfx_flush();            /* 整屏全刷 */
        else
            epd_gfx_flush_window(0, 0,
                                 epd_gfx_width(), epd_gfx_height());
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

/* 当前应显示页面的统一渲染入口：有词库走学习页，无词库走待机页
 * （LAN/配网退出与模式切换后的恢复路径均经此路由；menu_ui 恢复退出同） */
extern "C" void ui_render_current(void)
{
    study_mode_t m = study_mode_current();
    if (m == MODE_READER)
        /* 阅读模式恢复当前页（无书显示占位页），不回待机页 */
        ui_render_word(m, study_mode_seq_pos());
    else if (m == MODE_CHAT) {
        /* P2B 对话首帧：整屏全刷一次（状态栏 + 内容区）；环路内仅
         * 内容区局刷（ui_render_chat），进/出各一次全刷红线 */
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
        ui_render_word(m, 0);
    else
        standby_render_full();
}

/* P1 跟读评测三态屏显（pron_task 驱动；状态栏不动，内容区局刷 350ms
 * 级，符合对话环路禁全刷红线。反哺 P2B：chat_mode 状态区同策略）。
 * FAIL 态 total 复用透传错误码：-3=未听到话音，其余=网络/录音失败 */
extern "C" void ui_render_pron(pron_state_t st, int total, const char *engine)
{
    if (wifi_config_ui_is_active() || lan_server_is_active() ||
        menu_ui_is_active())
        return;                              /* 顶层覆盖层期间不绘制 */

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

/* P2B AI 对话屏显（chat_mode 任务驱动；状态区局刷同 ui_render_pron
 * 策略，环路内禁全刷红线。语音优先、屏幕克制：仅状态词 + 末句回复
 * ≤2 行（听不清时看屏）。三色面板 partial_enabled=false 零渲染，
 * 纯语音+震动（与待机页轮换停用同款 UX 降级先例） */
extern "C" void ui_render_chat(chat_state_t st, const char *text)
{
    if (!epd_gfx_partial_supported()) return;   /* 三色降级：纯语音+震动 */
    if (wifi_config_ui_is_active() || lan_server_is_active() ||
        menu_ui_is_active())
        return;                              /* 顶层覆盖层期间不绘制 */

    epd_gfx_fill_rect(0, UI_STATUS_H, epd_gfx_width(),
                      epd_gfx_height() - UI_STATUS_H, EPD_GFX_WHITE);

    switch (st) {
    case CHAT_STATE_IDLE:
        epd_gfx_draw_text(UI_MARGIN_X, UI_WORD_BASE, "AI Chat",
                          EPD_GFX_BLACK,
                          ui_fit_font("AI Chat", 4, UI_BODY_MAX_W));
        cjk_text_draw(UI_MARGIN_X, UI_BODY_TOP, UI_MEAN_LEVEL,
                      "按中键说话 · 长按中键退出", EPD_GFX_BLACK);
        break;
    case CHAT_STATE_RECORDING:
        epd_gfx_draw_text(UI_MARGIN_X, UI_WORD_BASE, "Listening...",
                          EPD_GFX_BLACK, 2);
        cjk_text_draw(UI_MARGIN_X, UI_BODY_TOP, UI_MEAN_LEVEL,
                      "请说话 · 停顿即发送 / 中键立即发", EPD_GFX_BLACK);
        break;
    case CHAT_STATE_UPLOADING:
        epd_gfx_draw_text(UI_MARGIN_X, UI_WORD_BASE, "Sending...",
                          EPD_GFX_BLACK, 2);
        break;
    case CHAT_STATE_THINKING:
        epd_gfx_draw_text(UI_MARGIN_X, UI_WORD_BASE, "Thinking...",
                          EPD_GFX_BLACK, 2);
        break;
    case CHAT_STATE_PLAYING:
        epd_gfx_draw_text(UI_MARGIN_X, UI_WORD_BASE, "Speaking",
                          EPD_GFX_BLACK, 2);
        cjk_text_draw(UI_MARGIN_X, UI_BODY_TOP, UI_MEAN_LEVEL,
                      "中键打断重说", EPD_GFX_BLACK);
        break;
    case CHAT_STATE_NETFAIL:
        epd_gfx_draw_text(UI_MARGIN_X, UI_WORD_BASE, "Offline",
                          EPD_GFX_BLACK, 2);
        cjk_text_draw(UI_MARGIN_X, UI_BODY_TOP, UI_MEAN_LEVEL,
                      "网络不可用 · 按中键重试", EPD_GFX_BLACK);
        break;
    }

    /* 末句回复 ≤2 行（状态词下方；text 空时跳过） */
    if (text && text[0])
        cjk_text_draw_wrap_page(UI_MARGIN_X, UI_BODY_TOP + 2 * UI_BODY_LH,
                                UI_BODY_MAX_W, UI_MEAN_LEVEL, UI_BODY_LH, 2,
                                0, text, EPD_GFX_BLACK);

    epd_gfx_flush_window(0, UI_STATUS_H, epd_gfx_width(),
                         epd_gfx_height() - UI_STATUS_H);
    LOG_I("chat ui state=%d", (int)st);
}

/* P5 幻影按键吞除武装标志：按键唤醒的会话置位（setup），on_button 吞掉
 * 唤醒后首个中键事件后清位（见 on_button 顶部注释） */
static bool s_wake_swallow_center = false;

/* 五向导航按键事件回调 */
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

    /* 快捷菜单激活时，按键全部转发（顶层覆盖层，与配网页同级语义） */
    if (menu_ui_is_active()) {
        menu_ui_on_button(id, event);
        return;
    }

    /* v1.2 T2.5 设置页激活时，按键全部转发（同级覆盖层，菜单退出后接替） */
    if (settings_ui_is_active()) {
        settings_ui_on_button(id, event);
        return;
    }

    /* P1 跟读评测期间：任意键取消录音 / 关闭结果屏（吞键，pron_task
     * 或 any_key 自恢复词卡；短事务期间不进菜单/翻词） */
    if (study_mode_pron_active() || study_mode_pron_ui_visible()) {
        study_mode_pron_any_key();
        return;
    }

    /* P2B AI 对话模式：按键全转发（中=开始/发送/打断重说；长按中或
     * RST=请求退出由编排层执行——模式状态归 study_mode_machine） */
    if (study_mode_current() == MODE_CHAT) {
        if (!chat_mode_on_button(id, event)) {
            haptic_event(HAPTIC_MODE);   /* 退出模式 50ms（进/出同档） */
            study_mode_exit_chat();
            ui_render_current();         /* 模式变化自然全刷回闪卡 */
        }
        return;
    }

    /* v1.2 T2.2 快速测验：按键全转发（上/下选项、中作答、SET 跳过、
     * RST 退出；作答反馈与首帧渲染由 quiz_on_button 内部编排） */
    if (study_mode_current() == MODE_QUIZ) {
        quiz_on_button(id, event);
        return;
    }

    /* 教材目录浏览（设计 §A2）：按键全转发（上下移动/中进入/RST 逐级
     * 返回，长按直退；模块内自管渲染，选词 confirm 经 seek 终结视图） */
    if (study_mode_current() == MODE_BROWSE) {
        browse_mode_on_button(id, event);
        return;
    }

    /* AI 语音查词（设计 §B2）：按键全转发（中=录音/提前停/确认，上下=
     * 候选移动，RST=重说）；退出请求由编排层执行——chat 同款编排 */
    if (study_mode_current() == MODE_VOICE) {
        if (!voice_search_on_button(id, event)) {
            haptic_event(HAPTIC_MODE);
            voice_search_request_exit();
            study_mode_exit_voice_search();
            ui_render_current();
        }
        return;
    }

    /* Wi-Fi 配置页激活时，按键全部转发 */
    if (wifi_config_ui_is_active()) {
        wifi_config_ui_on_button(id, event);
        return;
    }

    /* LAN 接收页 / AP portal 激活时，任意按键退出并回到学习界面
     * （portal 模式下 lan_portal_exit 关热点回 STA；均为幂等调用） */
    if (lan_server_is_active()) {
        lan_portal_exit();
        lan_server_leave_receive_page();
        ui_render_current();
        return;
    }

    /* 待机页激活时（词库为空），按键交给待机页处理 */
    if (standby_is_active()) {
        standby_on_button(id, event);
        return;
    }

    /* 长按功能集中在五键上：中=功能菜单，上=清残影，下=模式切换，
     * 左=AP 门户（隔离环境下 STA 页面不可达时的可靠通道），
     * 右=LAN 接收页 */
    if (event == BUTTON_EVENT_LONG_PRESS) {
        switch (id) {
        case NAV_CENTER:
            menu_ui_enter();
            return;
        case NAV_UP:
            LOG_I("user requested ghost-clear full refresh");
            refresh_force_full();
            return;
        case NAV_DOWN:
            /* 切换学习模式并重绘（ui_render_word 检测到模式变化自动全刷） */
            haptic_event(HAPTIC_MODE);   /* 模式切换 50ms（PRD 5.4） */
            ui_sfx_play(UI_SFX_MODE);    /* T1.6 模式切换音「滴--」 */
            study_mode_switch_next();
            ui_render_current();
            return;
        case NAV_LEFT:
            lan_portal_enter();
            return;
        case NAV_RIGHT:
            /* 幂等启动服务器并显示访问 URL */
            lan_server_enter_receive_page();
            return;
        case NAV_SET:
            /* 收藏/取消当前词（P1）：局部重绘内容区刷新 * 标记；
             * 阅读模式无“当前词”概念，不响应；
             * 收藏视图（MODE_COLLECTION）内=移出序列（after_uncollect
             * 收缩钳位，清空自动退回闪卡），2026-08-23 */
            if (study_mode_current() == MODE_READER) return;
            haptic_event(HAPTIC_REVIEW); /* 确认型操作归自评档 30ms（PRD 5.4 未单列） */
            learning_state_toggle_collect(study_mode_current_word_index());
            if (study_mode_current() == MODE_COLLECTION &&
                study_mode_after_uncollect()) {
                ui_render_current();   /* 清空退回闪卡或游标收缩，重绘当前页 */
                return;
            }
            ui_render_word(study_mode_current(),
                           study_mode_current_word_index());
            return;
        case NAV_RST:
            /* 临时视图进出三级判：错词本/收藏浏览内=退出，否则进错词本
             * （无错词 100ms 长震边界反馈，PRD 5.4） */
            if (study_mode_current() == MODE_WRONGBOOK) {
                study_mode_exit_wrongbook();
            } else if (study_mode_current() == MODE_COLLECTION) {
                study_mode_exit_collection();
            } else if (!study_mode_enter_wrongbook()) {
                haptic_event(HAPTIC_ERROR);
                ui_sfx_play(UI_SFX_ERR); /* T1.6 边界拒绝音「嘟-」 */
                return;
            }
            haptic_event(HAPTIC_MODE);
            ui_render_current(); /* 模式变化 -> 全刷重绘第一条 */
            return;
        default:
            return;
        }
    }

    /* 短按：上/下翻词（释义多页时先词内翻释义页），中=发音，
     * SET=遮蔽/揭晓释义，RST=回第一条；
     * 左=自评「忘记」Q1，右=自评「简单」Q5（FSRS 评分入 learning_state，
     * 错词本内答对自动移出，序列清空自动退回闪卡） */
    if (event != BUTTON_EVENT_SHORT_PRESS) return;

    /* 阅读模式短按路由（P3）：上/下=翻页，左/右=字号缩放，
     * RST=回第一页（“回到当前模式第一条”全局语义）；
     * 词相关动作（发音/自评/遮蔽）不适用，中/SET 忽略 */
    if (study_mode_current() == MODE_READER) {
        switch (id) {
        case NAV_UP:
            study_mode_handle_action(0);      /* 上一页 */
            return;
        case NAV_DOWN:
            study_mode_handle_action(1);      /* 下一页 */
            return;
        case NAV_LEFT:
            study_mode_reader_font_step(-1);  /* 字号缩小（震动由去抖层 20ms 覆盖） */
            return;
        case NAV_RIGHT:
            study_mode_reader_font_step(+1);  /* 字号放大 */
            return;
        case NAV_RST:
            study_mode_reset_cursor();        /* 回第一页 */
            return;
        default:
            return;
        }
    }

    /* 复习模式短按路由（2026-08-24，O3）：列表态=到期词紧凑词表，
     * 上/下=移动选择、中=进词卡详情、左/右=自评出队（游标钳位，
     * 对应底部提示行「中 详情 · 左/右 自评出队 · RST 设置」）、
     * RST=直达设置页（2026-08-27）、SET 无遮蔽语义忽略；空序列全忽略（空态页无交互
     * 对象，评分目标词索引无效）；详情态上/下/中/SET/RST 走下方
     * 通用词卡路由（上/下翻释义页/跨词翻卡），左/右自评改为
     * after_due_review 出队并回列表——序列清空由列表态空态页承载，
     * 避免空序列词卡取词 */
    if (study_mode_current() == MODE_REVIEW && !s_review_detail) {
        switch (id) {
        case NAV_UP:
            study_mode_handle_action(0);   /* 选择上一词（回绕，内部重绘） */
            return;
        case NAV_DOWN:
            study_mode_handle_action(1);   /* 选择下一词 */
            return;
        case NAV_CENTER:
            if (study_mode_seq_total() == 0) return;
            s_review_detail = true;       /* 进词卡详情 */
            ui_render_word(study_mode_current(),
                           study_mode_current_word_index());
            return;
        case NAV_LEFT:
        case NAV_RIGHT: {
            if (study_mode_seq_total() == 0) return;
            int q = (id == NAV_RIGHT) ? 5 : 1;
            learning_state_apply_quality(
                study_mode_current_word_index(), q);
            haptic_event(HAPTIC_REVIEW);   /* 自评提交 30ms（PRD 5.4） */
            ui_sfx_play(UI_SFX_RATE);      /* T1.6 自评提交音「滴答」 */
            study_mode_after_due_review(); /* REVIEW 恒 true：出队钳位 */
            ui_render_word(study_mode_current(),
                           study_mode_current_word_index());
            return;
        }
        case NAV_RST:
            study_mode_reset_cursor();     /* 回首行 */
            return;
        default:                           /* NAV_SET：列表态无遮蔽语义 */
            return;
        }
    }
    if (study_mode_current() == MODE_REVIEW && s_review_detail &&
        (id == NAV_LEFT || id == NAV_RIGHT)) {
        /* 详情态自评：出队 + 回列表（下词钳位高亮；清空→空态页） */
        int q = (id == NAV_RIGHT) ? 5 : 1;
        learning_state_apply_quality(study_mode_current_word_index(), q);
        haptic_event(HAPTIC_REVIEW);
        ui_sfx_play(UI_SFX_RATE);      /* T1.6 自评提交音「滴答」 */
        s_review_detail = false;
        study_mode_after_due_review();
        ui_render_word(study_mode_current(),
                       study_mode_current_word_index());
        return;
    }

    switch (id) {
    case NAV_UP:
        if (ui_mean_page_step(-1)) return;  /* 释义多页：词内上一页 */
        study_mode_handle_action(0);   /* prev */
        return;
    case NAV_DOWN:
        if (ui_mean_page_step(+1)) return;  /* 释义多页：词内下一页 */
        study_mode_handle_action(1);   /* next */
        return;
    case NAV_CENTER:
        study_mode_handle_action(3);   /* speak */
        return;
    case NAV_SET:
        study_mode_handle_action(2);   /* confirm：遮蔽/揭晓释义 */
        return;
    case NAV_RST:
        /* RST 短按直达设置页（2026-08-27 用户需求：音量等高频项快速
         * 触达；原「回当前模式首条」退役——低频功能，可由多次上键
         * 等价达成；quiz/AI 对话等临时视图的 RST 语义在前置分支不受影响） */
        settings_ui_enter();
        return;
    case NAV_LEFT:
        learning_state_apply_quality(study_mode_current_word_index(), 1);
        haptic_event(HAPTIC_REVIEW);   /* 自评提交 30ms（PRD 5.4） */
        ui_sfx_play(UI_SFX_RATE);      /* T1.6 自评提交音「滴答」 */
        if (study_mode_after_quality(1))
            ui_render_word(study_mode_current(),
                           study_mode_current_word_index());
        return;
    case NAV_RIGHT:
        learning_state_apply_quality(study_mode_current_word_index(), 5);
        haptic_event(HAPTIC_REVIEW);   /* 自评提交 30ms（PRD 5.4） */
        ui_sfx_play(UI_SFX_RATE);      /* T1.6 自评提交音「滴答」 */
        if (study_mode_after_quality(5))
            ui_render_word(study_mode_current(),
                           study_mode_current_word_index());
        return;
    default:
        return;
    }
}

/* 后台心跳 + OTA 检查任务。
 * 启动阶段：每 2s 轮询，联网即立即启动 LAN 直传服务（不设上限：
 *           即使路由器后启动/断电恢复，联网后也能尽快拉起服务）；
 * 之后转为 10 分钟周期：上报队列 flush + 首次注册 + 心跳 + OTA
 * 检查（含服务兜底重启，幂等） */
/* ============================================================
 * 云端同步凭据与上报 flush (P2)
 * 凭据链：NVS "inkword"/{api_url, dev_key} → sync_set_*；无 key 时
 * 联网后按 MAC 幂等注册（后端返回既有 ApiKey）并回写 NVS。
 * ============================================================ */

static void sync_credentials_load(void)
{
    char url[128], key[64];
    nvs_handle_t h;
    if (nvs_open("inkword", NVS_READONLY, &h) != ESP_OK) {
        sync_set_base_url(INKWORD_API_BASE);
        return;
    }
    size_t len = sizeof(url);
    if (nvs_get_str(h, "api_url", url, &len) == ESP_OK)
        sync_set_base_url(url);
    else
        sync_set_base_url(INKWORD_API_BASE);
    len = sizeof(key);
    if (nvs_get_str(h, "dev_key", key, &len) == ESP_OK)
        sync_set_device_key(key);
    nvs_close(h);
}

static void sync_try_register(void)
{
    if (sync_has_device_key()) return;

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[13];
    snprintf(mac_str, sizeof(mac_str), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    char key[64];
    if (sync_register(mac_str, NULL, key, sizeof(key)) == 0) {
        sync_set_device_key(key);
        nvs_handle_t h;
        if (nvs_open("inkword", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, "dev_key", key);
            nvs_commit(h);
            nvs_close(h);
        }
        LOG_I("device registered, key persisted");
    } else {
        LOG_W("register failed, retry next cycle");
    }
}

/* v2.0 绑定换发自愈（ADR-001 §五）：App 绑定设备后云端换发 ApiKey，
 * 旧钥即刻 401。清内存/NVS 钥 → sync_try_register 按 MAC 幂等重注册
 * 取回新钥（后端 register 返回既有记录的钥，即换发后的新钥）。网络
 * 未连/后端不可达时注册失败，key 保持空下周期再试（离线优先红线：
 * 本地学习全链路不依赖钥）。 */
static void sync_recover_auth(void)
{
    LOG_W("device key rejected (401), re-register by MAC");
    sync_set_device_key("");
    nvs_handle_t h;
    if (nvs_open("inkword", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "dev_key");
        nvs_commit(h);
        nvs_close(h);
    }
    sync_try_register();
}

/* 上报队列 flush：逐条发送（人手按键频次下 HTTP 开销可忽略；攒批优化
 * 待设备规模上来后）。无 cloudId 的词（本地导入）直接丢弃；任一条
 * 失败即停，队列保留待下周期重试（timestamp=0 由服务器落地时间代替） */
static void sync_flush_pending(void)
{
    int guard = learning_state_event_count();
    while (guard-- > 0) {
        lr_event_t ev;
        if (!learning_state_event_peek(0, &ev)) break;

        const WordEntry *w = word_parser_get(ev.word_idx);
        if (!w || !w->cloud_id[0]) {
            learning_state_event_drop(1); /* 本地词：无云端身份，事件无价值 */
            continue;
        }

        if (ev.quality >= 0) {
            ProgressItem it = {};   /* 全零初始化（quality/word_id/timestamp） */
            it.quality = (uint8_t)ev.quality;
            it.timestamp = 0;
            strncpy(it.word_id, w->cloud_id, sizeof(it.word_id) - 1);
            int rc = sync_push_progress(&it, 1);
            if (rc == SYNC_ERR_AUTH) { sync_recover_auth(); return; }
            if (rc != 0) return;
        } else {
            int rc = sync_push_collect(w->cloud_id, ev.collected);
            if (rc == SYNC_ERR_AUTH) { sync_recover_auth(); return; }
            if (rc != 0) return;
        }
        learning_state_event_drop(1);
    }
}

/* ============================================================
 * P5 静默心跳会话：RTC TIMER 唤醒后的极简启动路径（不返回）
 * 屏/SD/音频/学习状态全不初始化：墨水屏驻留末帧不碰 COG，
 * sync_flush_pending 的 guard=learning_state_event_count()=0（静态
 * 零初始化）自然空转——事件队列是内存态且仅由按键产生，入睡时已
 * 论证必空（见 power_enter_sleep 注释）。NVS 必须初始化（凭据/时钟
 * checkpoint 均在 NVS）。业务链：Wi-Fi 快连（10s 超时失败静默回睡，
 * 不重试不闪屏）→ HTTP Date 校时（standby_page 静态基准对无需
 * standby_init 即可写）→ 注册/上报/心跳/OTA 检查 → 回睡。
 * ============================================================ */
static void silent_heartbeat_session(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    wifi_manager_init();
    sync_credentials_load();

    /* 未配网的新设备（无凭据即入睡）：不白等超时直接回睡 */
    if (!wifi_has_saved_credentials()) {
        LOG_W("silent session: no wifi credentials, back to sleep");
        power_enter_sleep(PM_HEARTBEAT_PERIOD_S);
    }

    /* 连接为事件驱动异步（wifi_manager_init 内自动连已存网络），
     * 轮询等待：路由器在线典型 2~3s，离线等满 10s 静默回睡 */
    int waited_s = 0;
    while (!wifi_is_connected() && waited_s < PM_WAKE_WIFI_TIMEOUT_S) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        waited_s++;
    }
    if (!wifi_is_connected()) {
        LOG_W("silent session: wifi timeout (%ds), back to sleep", waited_s);
        power_enter_sleep(PM_HEARTBEAT_PERIOD_S);
    }

    /* HTTP Date 校时：刷新 standby 自治钟内存基准（回睡前由
     * power_enter_sleep 内 checkpoint 落 NVS，下级唤醒用新基准） */
    int64_t now = sync_fetch_http_time();
    if (now > 0) {
        standby_time_set(now);
        LOG_I("silent session: clock calibrated (epoch=%lld)", (long long)now);
    }

    /* 云端闭环（与 background_task 周期段同链）：幂等注册 + 上报 flush */
    sync_try_register();
    sync_flush_pending();
    int bat = max17048_percent();   /* T2.6：实数（模块不在位回退占位） */
    if (bat < 0) bat = 100;
    if (sync_heartbeat(bat, FW_VERSION) == SYNC_ERR_AUTH)
        sync_recover_auth();   /* 换钥后回睡，下个心跳周期新钥生效 */

    /* OTA 检查：升级成功即重启进新固件（走正常启动路径 ota_mark_valid） */
    char url[256], md5[64];
    int size = 0;
    if (ota_check_for_update(url, sizeof(url), md5, sizeof(md5), &size)) {
        LOG_I("OTA update found (silent session), size=%d", size);
        ota_perform_upgrade(url, md5);
    }

    power_enter_sleep(PM_HEARTBEAT_PERIOD_S);   /* 不返回 */
}

static void background_task(void *arg)
{
    (void)arg;
    while (!lan_server_is_running()) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (wifi_is_connected()) {
            lan_server_start(); /* 幂等 */
        }
    }
    const TickType_t period = pdMS_TO_TICKS(10 * 60 * 1000); /* 10 分钟 */
    int wx_poll_cnt = 2; /* 待机页天气轮询计数：初始 2 -> 首个周期即拉取 */
    while (1) {
        vTaskDelay(period);
        if (wifi_is_connected()) {
            lan_server_start(); /* 兜底：服务异常停止则重启（幂等） */

            /* 云端闭环（P2）：首次注册（幂等）+ 评分/收藏上报 flush */
            sync_try_register();
            sync_flush_pending();

            int bat = max17048_percent();   /* T2.6：实数（不在位回退占位） */
            if (bat < 0) bat = 100;
            if (sync_heartbeat(bat, FW_VERSION) == SYNC_ERR_AUTH)
                sync_recover_auth();   /* 新钥本周期即取回，下周期正常 */

            /* 待机页天气：每 3 个周期（约 30 分钟）拉取一次，失败下周期重试；
             * 仅待机页激活时拉取（学习页不耗流量） */
            if (standby_is_active() && ++wx_poll_cnt >= 3) {
                weather_info_t wx;
                if (sync_fetch_weather(&wx) == 0) {
                    standby_weather_update(&wx);
                    wx_poll_cnt = 0;
                } else {
                    wx_poll_cnt = 2;
                }
            }

            /* 顺带检查 OTA */
            char url[256], md5[64];
            int size = 0;
            if (ota_check_for_update(url, sizeof(url), md5, sizeof(md5), &size)) {
                LOG_I("OTA update found, size=%d", size);
                /* 自动升级可改为需用户确认 */
                ota_perform_upgrade(url, md5);
            }
        }
    }
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
    button_register_callback(on_button);
    boot_stamp("keys");

    /* 3. 刷新调度器：阈值取面板 desc.partial_count_full_refresh
     * （2026-08-26 wft0290 调优改：原硬编码 8 与 desc 脱钩；
     * 残影为单相快刷固有特性，wft0290 取 4 加频清除，全刷 3.4s
     * 洗净实测；待机页走独立 _n 阈值 12，见 standby_page.c） */
    const epd_panel_desc_t *pd = epd_panel_desc();
    refresh_scheduler_init(pd && pd->partial_count_full_refresh > 0
                               ? pd->partial_count_full_refresh : 8);

    /* 4. WiFi 联网（失败不阻塞主流程）；同步凭据（base URL / 设备 key）
     *    从 NVS 恢复到 sync_client，首次注册留待联网后 background_task */
    wifi_manager_init();
    sync_credentials_load();

    /* 4.5 Wi-Fi 配置：无凭据时自动开启 AP 配网门户
     *     （手机连 InkWord-Setup 热点后自动弹出配置页）；
     *     软键盘配置 UI 仍可长按 C 进入 */
    wifi_config_ui_init();
    if (!wifi_has_saved_credentials()) {
        LOG_W("no saved WiFi, starting AP portal");
        lan_portal_enter();
    }

    /* 4.6 BLE 配网服务（App 扫描发现/配网；失败仅告警，
     *     Portal 与软键盘配网路径不受影响）。
     *     默认禁用，根因见文件头 INKWORD_BLE_PROVISION 注释 */
#if INKWORD_BLE_PROVISION
    ble_provision_init();
#endif

    /* 5. 标记当前固件有效，防止 OTA 回滚 */
    ota_mark_valid();
    boot_stamp("wifi");

    /* 6. 加载词库（词池 PSRAM 化，2026-08-20）：按 MAX_WORDS 逐半降级
     *    分配，与阅读器书缓冲共享 8MB Octal；全部分配失败（极小概率）
     *    置空容量，词库空走待机页 */
    for (int cap = MAX_WORDS; cap > 0 && !s_word_pool; cap /= 2) {
        s_word_pool = (WordEntry *)heap_caps_malloc(
            (size_t)cap * sizeof(WordEntry), MALLOC_CAP_SPIRAM);
        if (s_word_pool) s_word_cap = cap;
        else LOG_W("word pool alloc %d entries failed, halving", cap);
    }
    LOG_I("word pool: %d entries x %uB = %uKB PSRAM",
          s_word_cap, (unsigned)sizeof(WordEntry),
          (unsigned)((size_t)s_word_cap * sizeof(WordEntry) / 1024));

    /* v1.3 T3.1：卡组扫描（无 manifest 退化为单默认卡组，开箱行为
     * 不变）；装载走 load_active_words 三级递降链路（与切书共用） */
    deck_manager_scan();
    if (s_word_pool) {
        int n = load_words_with_catalog();
        LOG_I("word DB ready: %d entries", n);
    } else {
        LOG_W("word pool alloc failed, no word DB");
    }
#if INKWORD_DEMO_WORDS
    /* 测试构建：内嵌词库也被排除时（如裁剪验证）的最后一道演示词 */
    if (s_word_pool && word_parser_get_count() == 0) {
        word_parser_load_demo(s_word_pool, s_word_cap);
        catalog_build();    /* 演示词路径同建索引（装载尾部口径统一） */
    }
#endif

    /* 6.5 本地学习状态（P1 错词本/收藏）：按卡组+词库规模锁定并从
     *     该组 NVS 键恢复（LR04）；必须先于 study_mode_init/首次渲染
     *     （错词序列与收藏标记依赖） */
    learning_state_init(word_parser_get_count(), deck_manager_active_id());
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
        ui_render_current();            /* READER 书页/占位页 或 待机页 */
    }
    boot_stamp("first-frame");

    /* 8. 启动后台任务（心跳/OTA） */
    xTaskCreate(background_task, "bg", 6 * 1024, NULL, 4, NULL);

    LOG_I("=== InkWord ready ===");
    boot_stamp("ready");
}

/* Arduino loop - 主循环（事件驱动；待机页分钟级心跳由 standby_tick 承载，
 * 非待机状态时零开销返回；学习状态脏标记静默 5s 后在非按键路径落盘） */
void loop()
{
    standby_tick();
    learning_state_maybe_save();  /* LR02 sparse 延迟保存（无脏零开销） */
    power_maybe_sleep();          /* P5：无操作超时且无禁睡条件则入睡（不返回） */
    vTaskDelay(pdMS_TO_TICKS(1000));
}
