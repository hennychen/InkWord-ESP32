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

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_WORD_LOADER_H */
