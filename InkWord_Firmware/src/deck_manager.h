/**
 * @file deck_manager.h
 * @brief 词书（Deck）卡组管理器（v1.3 T3.1，MENU_DESIGN 二期位兑现）
 *
 * SD 布局（计划 §五 T3.1 冻结）：
 *   /sdcard/decks/manifest.json   卡组清单（生成/下发协议见下）
 *   /sdcard/decks/<id>/words.json 各卡组词库（words.json 同格式）
 * 现有 /sdcard/words.json 兼容为默认卡组（id=""，扫描结果恒在 [0]）；
 * 无 manifest / 解析失败一律退化为单默认卡组（开箱行为不变）。
 *
 * manifest 协议（v1.3 制定；subject/payloadType 预留字段 v1.4 T4.2
 * 兑现——scan 解析入表、upsert 可写，T4.3 渲染分派消费 payload_type）：
 *   { "version": 1,
 *     "decks": [ { "id": "cet4", "name": "CET-4 核心",
 *                  "subject": "en", "payloadType": "word-card",
 *                  "file": "cet4/words.json", "count": 2500 } ] }
 *
 * 活跃卡组记录 NVS "inkword"/deck_active（字符串 id，""=默认）；
 * 切换的重载编排（词库 load + learning_state_reload + reader scope +
 * 渲染刷新）在 main.cpp deck_flow_switch，本模块只管清单与活跃记录。
 *
 * id 长度 ≤7：阅读进度键后缀预算（NVS 键名 15 上限，rd_page_(7)+7）。
 */
#ifndef INKWORD_DECK_MANAGER_H
#define INKWORD_DECK_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DECK_MAX       16   /**< 卡组数上限（含默认；菜单单页可见性预算） */
#define DECK_ID_MAX    7    /**< id 长度上限（NVS 进度键后缀预算） */

/** 卡组信息（静态表，无堆分配） */
typedef struct {
    char id[DECK_ID_MAX + 1];    /**< 短 id（""=默认卡组，恒在 [0]） */
    char name[32];               /**< 显示名（菜单列表/主菜单徽标） */
    char file[64];               /**< 词库绝对路径（默认卡组="", 用 words.json/内嵌） */
    int  count;                  /**< manifest 标注词条数（信息展示用） */
    char subject[8];             /**< 科目代码（v1.4 T4.2 兑现；"en" 缺省） */
    char payload_type[16];       /**< 版式类型（T4.3 渲染分派键；"word-card" 缺省） */
} deck_info_t;

/**
 * @brief 扫描卡组清单（setup 词库装载前调用一次；菜单进入时可重扫）。
 *        读 /sdcard/decks/manifest.json；SD 缺失/无清单/解析失败均安全
 *        退化为仅默认卡组。
 * @return 卡组数（≥1，含默认 [0]）。
 */
int deck_manager_scan(void);

/** 已扫描卡组数（含默认；未扫描返回 1） */
int deck_manager_count(void);

/** 取卡组信息（[0] 恒为默认卡组；越界返回 NULL） */
const deck_info_t *deck_manager_at(int idx);

/** 活跃卡组 id（""=默认；NVS deck_active 读取，未记录=默认） */
const char *deck_manager_active_id(void);

/** 活跃卡组索引（菜单光标预定位用） */
int deck_manager_active_index(void);

/** 活跃卡组显示名 */
const char *deck_manager_active_name(void);

/** 活跃卡组科目代码（默认 "en"；AI 科目维度/T4.3 渲染分派用） */
const char *deck_manager_active_subject(void);

/** 活跃卡组版式类型（默认 "word-card"；T4.3 card_layout 分派键） */
const char *deck_manager_active_payload_type(void);

/**
 * @brief 活跃词库文件路径。
 * @return 非 NULL=SD 卡组文件（/sdcard/decks/...）；NULL=默认卡组
 *         （调用方走 words.json 存在则加载、否则内嵌兜底的既有链路）。
 */
const char *deck_manager_active_file(void);

/**
 * @brief 切换活跃卡组（仅写 NVS deck_active；词库重载由调用方编排）。
 * @return 0 成功；-1 索引越界。
 */
int deck_manager_switch(int idx);

/**
 * @brief 新增/更新卡组条目并写回 manifest.json（v1.3 T3.4 LAN 推送）。
 *        调用方先落词库文件，本函数负责清单登记（file 固定 <id>/words.json
 *        惯例路径）+ 重扫刷新内存表。manifest 写回失败返回 -1（词库文件
 *        已在但未登记，重启后不可见，调用方可提示重试）。
 * @param id   短 id（1~7 字符，[0-9a-zA-Z_-]）。
 * @param name 显示名（空则回退 id）。
 * @param count 词条数（信息展示用，0=未知）。
 * @param subject 科目代码（NULL/空 = "en"；T4.4 古诗文 = "zh"）。
 * @param payload_type 版式类型（NULL/空 = "word-card"；T4.3 分派键）。
 * @return 0 成功；-1 参数/IO 失败。
 */
int deck_manager_upsert(const char *id, const char *name, int count,
                        const char *subject, const char *payload_type);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_DECK_MANAGER_H */
