/**
 * @file settings_ui.h
 * @brief 设置页覆盖层（v1.2 T2.5，MENU_DESIGN 二期位兑现）
 *
 * 镜像 menu_ui 覆盖层范式：enter/is_active/on_button 三件套，无 init
 * （静态零初始化）、无阻塞（按键回调上下文同步绘制）。菜单 [系统] 组
 * 「设置」项进入（先 menu_ui_exit 后 enter，双激活防线同菜单子功能）。
 *
 * 行项：每日新词量（±5 循环，与 daily_plan 共用 set_daily 键）/ 发音
 * [开关] / 震动 [开关] / 字号 [标准|大字|特大]（2026-08-27 三档化，
 * 意图存档、渲染层按布局档位钳位）/ 单词大小 [大|中|小]（2026-08-27
 * P1b：ui_fit_font 起步档 4/3/2，超宽降级机制不变）/ 粗细 [标准|
 * 加粗]（2026-08-27 P2：FreeSans/Bold 表切换，仅英文/ASCII 路径） /
 * 测验快答 [开关]（v1.5
 * T5.1：2×2 方向直选，默认关=纵列基线；T3 听音题恒直选不受此键控）
 * / 考试倒计时（v1.5 T5.5）+ 屏幕方向 [默认|竖屏|横屏]（2026-08-26
 * 增，即改即生效：ui_apply_rotation 重建画布后本页全刷重排）
 * + 音量（2026-08-27：中键 +10 循环 0→10→…→100→0；连续微调走
 * 菜单 [系统]「音量」页上/下键，两入口共用同一状态即时生效）
 * + 面板型号（P1 收官 2026-09-02 运行期选屏：默认→注册表顺序
 * 循环；存面板注册名字符串，切换重启生效——画布/fb/布局档均
 * 派生自 desc 不热切；错选屏不亮的恢复=按住 RST 侧键上电忽略
 * 覆盖，见 epd_driver_init；紧凑档值列降级「自定」二态，完整
 * 型号串口 LOG / device-info 可查）
 * + 快捷键（2026-09-03：值列 = 已自定义键位数/「默认」；中键进入
 * 长按键配置子模式——上/下/左/右/SET/RST 六槽位 × shortcut_map
 * 动作目录循环切换，即改即存 NVS；映射执行与守卫见 main.cpp
 * shortcut_try_long 与 shortcut_map.h）。
 *
 * 取值 API 与 UI 分层：settings_audio/haptic_enabled 供 haptic.c /
 * ui_sfx.c / study_mode_machine.c 门控（纯 NVS 惰性缓存，无 UI 依赖，
 * 按键高频路径零 flash 读）；settings_font_mode 供 main.cpp 排版宏与
 * reader_engine 默认字号档修正；settings_quiz_grid 供 main.cpp 测验
 * 按键路由与版式分派。
 *
 * NVS 键（"inkword" 命名空间追加，不动既有键）：
 *   set_daily u8（daily_plan 定义）/ set_audio u8 / set_haptic u8 /
 *   set_font u8（0=标准/1=大字/2=特大，旧值 0/1 语义不变零迁移） /
 *   set_word u8（0=大/1=中/2=小，单词 fit 起步档 4/3/2） /
 *   set_bold u8（0=标准/1=加粗，FreeSans/Bold 表切换） /
 *   set_quizgrid u8 / set_rot u8（0=跟随面板默认 /
 *   1=竖屏 / 2=横屏，意图相对面板默认方向表达，不存绝对旋转——
 *   同一键跨面板重编译语义不漂移；映射见 main ui_apply_rotation）
 *   / set_vol u8（0~100 步进10，默认 75=0dB 历史听感；es8311 驱动
 *   内同步保存，dac_start 起播回写，重启后由 main audio_init 后同步）
 *   / set_panel str（P1 运行期选屏：面板注册名，epd_driver_init
 *   读键覆盖 EPD_PANEL_DEFAULT_ID；缺失=跟随构建默认，「默认」选项
 *   删键——deck_active 同哲学）
 *   / sk_up/sk_dn/sk_lf/sk_rt/sk_set/sk_rst u8（2026-09-03 长按
 *   快捷键：sk_action_t 枚举值，缺省=出厂动作；见 shortcut_map.h）
 *   ——均默认开/标准/跟随（键缺失=默认，不写默认值）。
 */
#ifndef INKWORD_SETTINGS_UI_H
#define INKWORD_SETTINGS_UI_H

#include <stdbool.h>
#include "button_handler.h"
#include "page_router.h"  /* T1.4：page_t（g_settings_ui_page 导出） */

#ifdef __cplusplus
extern "C" {
#endif

/** 进入设置页（菜单「设置」项入口）。同步绘制并全刷；已激活幂等忽略。 */
void settings_ui_enter(void);

/** 按键转发接口（激活期间由 main 按键回调调用，同步处理）。 */
void settings_ui_on_button(nav_key_t id, button_event_t event);

/**
 * @brief T1.4 页面协议实例（enter=settings_ui_enter；经 page_router_push
 *        入栈：菜单设置项/RST 短按直达两个入口）。
 */
extern const page_t g_settings_ui_page;

/* ---- 取值 API（门控层，任何模块可调；惰性缓存，无 init）---- */

/** 发音开关（set_audio，默认开）：haptic/发音/提示音门控。 */
bool settings_audio_enabled(void);

/** 震动开关（set_haptic，默认开）：haptic_event 入口门控。 */
bool settings_haptic_enabled(void);

/** 字号档（set_font）：0=标准（档位默认）/ 1=大字 / 2=特大。学习页
 *  正文按档位钳位映射（TINY/SMALL 特大等价大字，见 main UI_MEAN_LEVEL）；
 *  阅读器仅影响无记忆默认级（+1 后钳位，书内 rd_font 记忆优先）。 */
int settings_font_mode(void);

/** 单词字号偏好（set_word，2026-08-27 P1b）：0=大(24pt)/1=中(18pt)/
 *  2=小(14pt)，ui_fit_font 起步档，超宽自动降级机制不变；默认 0=
 *  历史行为（start 4）。 */
int settings_word_size(void);

/** 粗细开关（set_bold，2026-08-27 P2，默认关）：仅英文/ASCII 路径
 *  （epd_gfx_set_bold 切 FreeSans/Bold 表，单词/状态栏/菜单）；CJK
 *  点阵与 IPA 音标行不受影响。setter 即时 apply，重启 main 同步。 */
bool settings_bold_enabled(void);

/** 测验快答（set_quizgrid，v1.5 T5.1）：false=纵列（P1 基线） /
 *  true=2×2 方向直选。T3 听音题恒直选（中键留给重播），不受此键控。 */
bool settings_quiz_grid(void);

/** 屏幕方向（set_rot，2026-08-26）：0=跟随面板默认（出厂横/竖持机
 *  方向）/ 1=竖屏 / 2=横屏。意图相对面板默认表达，绝对旋转映射与
 *  生效链（画布重建/布局失效/页表重排）见 main ui_apply_rotation。 */
int settings_rotation_mode(void);

/** 音量（set_vol，2026-08-27）：0~100 步进 10，默认 75（=0xBF 历史听感）。
 *  setter 即时 apply es8311（codec 未起播时静默，dac_start 回写）。 */
int settings_volume(void);

/** 音量 setter：钳 0~100，NVS 持久化 + es8311 即时生效。 */
void settings_volume_set(int v);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_SETTINGS_UI_H */
