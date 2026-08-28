/**
 * @file catalog_index.h
 * @brief 教材目录索引（2026-08-28，教材目录浏览+语音查词设计 §A1）
 *
 * 词库装载后一次遍历构建两级目录：grade（年级）→ unit（教材单元）
 * → 词库序号数组。数据源为 WordEntry.source/grade 现有字段，零
 * words.json 协议改动、零 WordEntry 增长（双红线）。
 *
 * 解析规则（2026-08-28 单元细分增强，宽松兼容历史数据）：
 *   - 单元名：source 中最后一个 "unit"（不区分大小写）起截到结尾并
 *     回看 Starter 前缀（"人教版 九年级 Unit 5 What…" → "Unit 5 What…"
 *     话题名保留，前置版本/年级信息由 grade 层承载；"Starter Unit 1
 *     Good morning!" → 整段含 Starter，不与正课单元合并）；无 "unit"
 *     则整个 source 去重为单元名；source 空归 "(未分类)"。
 *   - 年级排序：预定义序表（小学→七上→九下→考纲拓展→中考/高考/
 *     初中/高中）模糊包含优先，未匹配按 UTF-8 字典序尾置；grade 全
 *     空的词归 "(其他)" 桶。
 *   - 单元排序：[Starter] Unit N（含话题名后缀）按数值序，Starter 键
 *     = N 排正课（N+1000）前且各自升序；非 Unit 名按字典序；桶内词
 *     条保持词库原序（导出顺序即教材顺序，稳定）。
 *
 * 内存：词序号数组 uint16_t×N（4000 词 ≈8KB，PSRAM 一次分配，
 * 词库切换时 catalog_release 重建）+ 桶头静态表（≈10KB 常驻预算内）。
 *
 * 纯 C 可 native 测试：catalog_build_from 泛化入口不依赖 word_parser
 * 运行期状态（设备端便捷封装 catalog_build 内部转发）。
 */
#ifndef INKWORD_CATALOG_INDEX_H
#define INKWORD_CATALOG_INDEX_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CATALOG_GRADE_MAX  16   /**< 年级桶上限（超出的并入"(其他)"） */
#define CATALOG_UNIT_MAX   16   /**< 单年级单元桶上限（超出并入"(未分类)"） */
#define CATALOG_NAME_MAX   48   /**< 桶名缓冲（单元名含话题如 "Unit 3 Could you…"，UTF-8 安全截断） */

/** 桶头（静态表项，无堆） */
typedef struct {
    char    name[CATALOG_NAME_MAX];  /**< 桶显示名（年级名/单元名） */
    int32_t start;                   /**< entries 数组内起始下标 */
    int32_t len;                     /**< 词条数 */
} catalog_bucket_t;

/**
 * @brief 从词条数组构建目录索引（native 测试入口，泛化解耦）。
 *        重复调用自动释放旧索引（幂等重建）。
 * @param n     词条数
 * @param words 词条数组（设备端 = word_parser 词池）
 * @return 年级桶数（≥0，0=空词库）；<0 内存失败（索引清空安全）。
 */
int catalog_build_from(int n, const void *words);  /* WordEntry 前向解耦 */

/** 设备端便捷封装：catalog_build_from(word_parser_get_count(), 词池) */
int catalog_build(void);

/** 释放索引（词库切换/装载前调用；空索引安全） */
void catalog_release(void);

/** 索引是否有效（build 成功后 true；release/失败后 false） */
bool catalog_ready(void);

/** 年级桶数 */
int catalog_grade_count(void);

/** 年级桶（越界返回 NULL） */
const catalog_bucket_t *catalog_grade(int g);

/** 年级名（越界返回 "(其他)" 兜底串） */
const char *catalog_grade_name(int g);

/** 指定年级下的单元桶数 */
int catalog_unit_count(int g);

/** 单元桶（越界返回 NULL） */
const catalog_bucket_t *catalog_unit(int g, int u);

/**
 * @brief 单元内词库序号数组（browse 词表直读）。
 * @param g,u   年级/单元下标
 * @param out_n 出参：词条数
 * @return 序号数组首址（静态索引内，索引重建前有效）；越界 NULL。
 */
const uint16_t *catalog_unit_entries(int g, int u, int *out_n);

/**
 * @brief 单元名解析（公开供测试）：source 提取单元显示名。
 * @param src    教材来源串（可空）
 * @param out    输出缓冲
 * @param out_sz 缓冲大小（超长截断）
 */
void catalog_unit_name_of(const char *src, char *out, int out_sz);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_CATALOG_INDEX_H */
