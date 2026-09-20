/**
 * @file word_loader.h
 * @brief 活跃词库装载链路（v1.3 T3.1 / T1.3 自 main.cpp 迁出，修 A1）
 *
 * 词池 PSRAM 分配 + 三级递降装载（活跃卡组 → SD words.json → 内嵌
 * 兜底）+ 目录索引构建。setup 启动与 deck_flow_switch 切书共用链路。
 * 词条消费方一律经 word_parser_get()（词池指针不出模块）。
 */
#ifndef INKWORD_WORD_LOADER_H
#define INKWORD_WORD_LOADER_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 词池分配（逐半降级）+ 卡组扫描 + 三级装载 + 演示词兜底：
 *  原 setup 步骤 6 整段函数化（T1.3，行为零变化） */
void word_loader_init(void);

/** 词库装载 + 目录索引（设计 §A1）：装载链路统一挂载点，setup 与
 *  deck_flow_switch 共用（词库切换时 catalog_build 内部 free 重建）。
 *  返回词条数（<0 全失败；内嵌兜底常在 rodata，末级必达） */
int load_words_with_catalog(void);

/** 最近一次装载命中层级（切书编排 vs 兑底区分判据）：切书时
 *  active deck 文件解析失败但兑底词库非空（返回 >0）曾被误判
 *  "切换成功"，NVS/学习状态污染到坏 deck 键——编排方以此枚举
 *  校验目标 deck 真正装载 */
typedef enum {
    WSRC_NONE = 0,   /**< 尚未装载 */
    WSRC_DECK,       /**< 活跃卡组文件命中 */
    WSRC_SD,         /**< SD 根 words.json 兑底 */
    WSRC_EMBED,      /**< 出厂内嵌兑底（末级必达） */
} word_loader_src_t;

word_loader_src_t word_loader_last_source(void);

/** R3.1 云端词库重载：从云端 JSON 缓冲重新装载词池。
 *  成功后重建目录索引。返回词条数（<0 失败）。
 *  全量替换词池，替换前/后调用 learning_state_pre/post_remap
 *  按 cloudId 匹配迁移学习状态（FSRS/收藏/墨封/连错）。 */
int word_loader_reload_from_cloud(const char *json, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_WORD_LOADER_H */
