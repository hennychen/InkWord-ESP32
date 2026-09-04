/**
 * @file shortcut_map.c
 * @brief 长按快捷键映射实现（设计见 shortcut_map.h 边界约定）
 *
 * 惰性缓存（settings_ui 取值 API 同款）：-1 未加载 → 首读 NVS；
 * setter 即写即更。无 init 无任务（按键高频路径零 flash 读）。
 * 「默认」不落盘：NVS 键缺失即 DEFAULT，回绕步进到 0 时删键
 * （deck_active/set_panel 同哲学，重烧/清分区不残留脏值）。
 */
#include "shortcut_map.h"
#include "settings_keys.h"   /* P2b：NVS 键权威表 */

#include "nvs.h"

#include <stdbool.h>
#include <stddef.h>   /* NULL */

/* 可定制槽位表：nav_key_t → NVS 键（CENTER 不在表内=不可覆盖） */
static const char *key_of_slot(nav_key_t key)
{
    switch (key) {
    case NAV_UP:    return NVS_KEY_SK_UP;
    case NAV_DOWN:  return NVS_KEY_SK_DN;
    case NAV_LEFT:  return NVS_KEY_SK_LF;
    case NAV_RIGHT: return NVS_KEY_SK_RT;
    case NAV_SET:   return NVS_KEY_SK_SET;
    case NAV_RST:   return NVS_KEY_SK_RST;
    default:        return NULL;   /* CENTER（菜单锚点）等不可定制 */
    }
}

/* 显示名（值列 ≤4 全角字符预算；按键说明页出厂文案同词表） */
static const char *s_names[SK_ACT_COUNT] = {
    "默认", "无", "菜单", "换模式", "收藏夹", "错词本", "语音查词",
    "AI对话", "测验", "目录", "阅读", "清残影", "AP门户", "LAN页",
    "星标", "设置", "发音", "墨封", "书架",
};

/* ---- 惰性缓存 ---- */

static int8_t s_map[NAV_KEY_COUNT] = { -1, -1, -1, -1, -1, -1, -1 };

static sk_action_t load_slot(nav_key_t key)
{
    const char *nvs_key = key_of_slot(key);
    if (!nvs_key) return SK_ACT_DEFAULT;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        bool hit = nvs_get_u8(h, nvs_key, &v) == ESP_OK;
        nvs_close(h);
        /* 脏值防御：越界/未知枚举回落默认（不写回，重烧清分区自愈） */
        if (hit && v < SK_ACT_COUNT) return (sk_action_t)v;
    }
    return SK_ACT_DEFAULT;
}

sk_action_t shortcut_get(nav_key_t key)
{
    if (key < 0 || key >= NAV_KEY_COUNT) return SK_ACT_DEFAULT;
    if (s_map[key] < 0) s_map[key] = (int8_t)load_slot(key);
    return (sk_action_t)s_map[key];
}

void shortcut_set(nav_key_t key, sk_action_t act)
{
    const char *nvs_key = key_of_slot(key);
    if (!nvs_key || act < 0 || act >= SK_ACT_COUNT) return;

    if (act == SK_ACT_DEFAULT) {   /* 默认=删键（不写默认值哲学） */
        s_map[key] = (int8_t)SK_ACT_DEFAULT;
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_key(h, nvs_key);
            nvs_commit(h);
            nvs_close(h);
        }
        return;
    }

    s_map[key] = (int8_t)act;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, nvs_key, (uint8_t)act);
        nvs_commit(h);
        nvs_close(h);
    }
}

sk_action_t shortcut_action_next(sk_action_t cur)
{
    return (cur < 0 || cur >= SK_ACT_COUNT - 1) ? SK_ACT_DEFAULT
                                                : (sk_action_t)(cur + 1);
}

const char *shortcut_action_name(sk_action_t act)
{
    return (act > SK_ACT_DEFAULT && act < SK_ACT_COUNT) ? s_names[act]
                                                        : s_names[SK_ACT_DEFAULT];
}

sk_action_t shortcut_factory_action(nav_key_t key)
{
    /* 出厂映射（静态，不读 NVS）：改出厂长按须同步 main.cpp
     * base_page_on_button 长按 switch 与本表（双源一致） */
    switch (key) {
    case NAV_CENTER: return SK_ACT_MENU;
    case NAV_UP:     return SK_ACT_GHOST;
    case NAV_DOWN:   return SK_ACT_MODE;
    case NAV_LEFT:   return SK_ACT_PORTAL;   /* 出厂 AP 门户（墨封由右键自评简单联动） */
    case NAV_RIGHT:  return SK_ACT_LAN;
    case NAV_SET:    return SK_ACT_COLLECT;
    case NAV_RST:    return SK_ACT_WRONGBOOK;
    default:         return SK_ACT_NONE;
    }
}

int shortcut_custom_count(void)
{
    /* 6 槽全量预热（一次性 ≤6 次 NVS 读，设置页进入时调用可接受） */
    static const nav_key_t slots[6] = {
        NAV_UP, NAV_DOWN, NAV_LEFT, NAV_RIGHT, NAV_SET, NAV_RST,
    };
    int n = 0;
    for (int i = 0; i < 6; i++)
        if (shortcut_get(slots[i]) != SK_ACT_DEFAULT) n++;
    return n;
}
