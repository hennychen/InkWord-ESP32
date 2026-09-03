/**
 * @file shortcut_map.h
 * @brief 用户自定义长按快捷键映射（2026-09-03，用户需求「自由定义按键」）
 *
 * 可定制槽位 = 五向键 + SET/RST 的长按共 6 个（上/下/左/右/SET/RST）；
 * 中键长按 = 功能菜单为全设备发现性锚点，固定不可覆盖。短按承载
 * 翻词/发音/遮蔽/自评/设置直达等学习主链路语义，不开放定制
 * （7 键 × 2 态 = 14 槽位中仅长按 6 槽开放，误配置不可自锁）。
 *
 * 动作执行不驻本模块：main.cpp shortcut_exec 镜像 menu_ui act_* 与
 * base_page_on_button 出厂长按编排（本模块只管「哪个键→哪个动作」
 * 的映射存取，无 UI 无业务依赖，纯 C 可 native-test）。
 *
 * 守卫约定（main.cpp shortcut_try_long 内聚，改语义须同步此处注释）：
 *   - 错词本/收藏视图内 RST 长按 = 退出视图、SET 长按 = 移出收藏，
 *     临时视图逃生语义优先于用户映射，不可覆盖；
 *   - SK_ACT_DEFAULT（键缺失）= 跟随出厂动作（不写默认值的 NVS 哲学，
 *     deck_active/set_panel 同款）。
 *
 * NVS 键（settings_keys.h 权威表登记）：sk_up/sk_dn/sk_lf/sk_rt/
 * sk_set/sk_rst（u8 = sk_action_t 枚举值）。
 */
#ifndef INKWORD_SHORTCUT_MAP_H
#define INKWORD_SHORTCUT_MAP_H

#include "button_handler.h"   /* nav_key_t */

#ifdef __cplusplus
extern "C" {
#endif

/** 可绑定动作目录（值即 NVS 存储值；0=默认须保持，勿重排） */
typedef enum {
    SK_ACT_DEFAULT = 0,  /**< 跟随出厂动作（键缺失=默认） */
    SK_ACT_NONE,         /**< 无动作（禁用该键长按） */
    SK_ACT_MENU,         /**< 功能菜单 */
    SK_ACT_MODE,         /**< 换模式 */
    SK_ACT_COLLECTION,   /**< 收藏列表（临时视图） */
    SK_ACT_WRONGBOOK,    /**< 错词本（临时视图） */
    SK_ACT_VOICE,        /**< 语音查词（临时视图） */
    SK_ACT_CHAT,         /**< AI 对话（自由模式） */
    SK_ACT_QUIZ,         /**< 快速测验（临时视图） */
    SK_ACT_BROWSE,       /**< 教材目录（临时视图） */
    SK_ACT_READER,       /**< 阅读模式 */
    SK_ACT_GHOST,        /**< 清残影全刷 */
    SK_ACT_PORTAL,       /**< AP 配网门户 */
    SK_ACT_LAN,          /**< LAN 接收页 */
    SK_ACT_COLLECT,      /**< 星标当前词（收藏切换） */
    SK_ACT_SETTINGS,     /**< 设置页 */
    SK_ACT_SPEAK,        /**< 当前词发音 */
    SK_ACT_COUNT
} sk_action_t;

/**
 * @brief 查键位映射（惰性缓存首读 NVS；CENTER/越界恒 DEFAULT）。
 */
sk_action_t shortcut_get(nav_key_t key);

/**
 * @brief 写键位映射并持久化（钳位到合法枚举；CENTER 无操作）。
 */
void shortcut_set(nav_key_t key, sk_action_t act);

/**
 * @brief 动作循环步进（设置页中键用）：cur 的下一个合法动作，回绕 0。
 */
sk_action_t shortcut_action_next(sk_action_t cur);

/**
 * @brief 动作显示名（设置页值列/按键说明页共用词表，UTF-8 短文案）。
 */
const char *shortcut_action_name(sk_action_t act);

/**
 * @brief 出厂长按映射（静态，不读 NVS；与 main.cpp base_page_on_button
 *        长按 switch 一一对应，CENTER=菜单锚点）。按键说明页对
 *        DEFAULT 槽位显示出厂动作名（与设置页/说明页同词表）。
 */
sk_action_t shortcut_factory_action(nav_key_t key);

/**
 * @brief 已自定义（非 DEFAULT）键数（设置页「快捷键」行值列）。
 */
int shortcut_custom_count(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_SHORTCUT_MAP_H */
