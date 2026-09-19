/**
 * @file bookmark_mgr.h
 * @brief 书签管理（2026-09-05 阅读器增强阶段三；大屏原样移植）
 *
 * 支持手动添加/移除书签，书签列表跳转。每本书最多 32 个书签，
 * 按页码升序排列，NVS blob 持久化（键名按书 signature 区分）。
 *
 * SET 短按 = 当前页添加/移除书签（切换式，reader_page 编排）；
 * 阅读器菜单「书签」→ 子视图显示，上/下选择，中键跳转。
 */
#ifndef INKWORD_BOOKMARK_MGR_H
#define INKWORD_BOOKMARK_MGR_H

#include <stdint.h>
#include <stdbool.h>
#include "page_router.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 单枚书签 */
typedef struct {
    int      page;            /**< 书签页码 */
    uint32_t byte_offset;     /**< 字节偏移（精确定位） */
    char     note[32];        /**< 备注（空串=无备注） */
} bookmark_t;

#define BOOKMARK_MAX 32       /**< 每本书最多书签数 */

/** 初始化/切换书时调用：按书 signature 从 NVS 恢复书签表 */
void bookmark_mgr_load(uint32_t signature);

/** 持久化当前书签表到 NVS */
void bookmark_mgr_save(void);

/** 添加书签（当前页；已存在则不重复添加） @return 0 成功；-1 已满 */
int bookmark_add(int page, const char *note);

/** 移除当前页书签 @return 0 成功；-1 不存在 */
int bookmark_remove(int page);

/** 当前页是否有书签 */
bool bookmark_exists(int page);

/** 书签总数 */
int bookmark_count(void);

/** 第 idx 枚书签（按页码升序；越界返回 NULL） */
const bookmark_t *bookmark_at(int idx);

/** 跳到第 idx 枚书签的页码 @return 页码；越界返回 0 */
int bookmark_jump(int idx);

/** 清空所有书签（切书时调用） */
void bookmark_mgr_clear(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_BOOKMARK_MGR_H */
