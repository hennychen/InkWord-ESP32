/**
 * @file reader_engine.h
 * @brief 阅读模式引擎 (PRD 里程碑 P3，2026-08-20)
 *
 * 职责：SD/内置书加载（整本入 PSRAM）→ UTF-8 逐字解码 → 墨迹盒变宽排版
 * （中文全宽 / ASCII 半宽自动）→ 按字号级建页表 → 内容区渲染 → NVS 进度。
 * 页游标由 study_mode_machine 持有（READER 模式下序列=页序列），
 * 本模块只提供页数查询 / 渲染 / 字号切换（保持阅读位置）/ 进度恢复。
 *
 * 字号级：0=16px / 1=20px(默认) / 2=24px，字形取自 cjk_font 三级点阵；
 * 切级重建页表，按“当前页首字符字节偏移”重新定位页码。
 *
 * 书来源：优先 /sdcard/books/ 下第一个 .txt（UTF-8）；
 * INKWORD_DEMO_BOOK=1 构建时使用内置演示书（无 SD 验证按键/排版用）。
 */
#ifndef INKWORD_READER_ENGINE_H
#define INKWORD_READER_ENGINE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化：找书 + 整本入 PSRAM + 按默认字号级建页表 + 恢复字号级。
 *        无书时引擎保持未就绪（渲染走占位页），不阻塞其他模式。
 * @return 0 就绪；-1 无书/加载失败；-2 内存不足
 */
int reader_engine_init(void);

/** 是否有可读书（无书时进入阅读模式显示占位提示页） */
bool reader_ready(void);

/** 当前字号级总页数（未就绪返回 0） */
int reader_page_count(void);

/** 当前字号级（0/1/2；未就绪返回默认 1） */
int reader_font_level(void);

/**
 * @brief 切换字号级（dir=+1 放大 / -1 缩小，循环），重建页表。
 * @param cur_page 当前页码（模式机游标）
 * @return 新页码（按“cur_page 页首字符偏移”在 新页表中就近定位）
 */
int reader_font_step(int dir, int cur_page);

/**
 * @brief NVS 恢复上次阅读页（书签名匹配才有效）。
 * @return 页码；-1 无有效记录
 */
int reader_progress_page(void);

/**
 * @brief 渲染第 page 页到内容区（状态栏以下整幅，逐页全刷由调用方控制）
 *        并自动保存进度（页码 + 字号级，NVS "inkword"/rd_*）。
 */
void reader_render_page(int page);

/** 无书占位页：提示书籍目录与格式（阅读模式仍可进，按键翻页无效果） */
void reader_render_placeholder(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_READER_ENGINE_H */
