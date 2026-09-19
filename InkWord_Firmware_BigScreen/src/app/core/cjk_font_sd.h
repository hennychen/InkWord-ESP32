/**
 * @file cjk_font_sd.h
 * @brief SD 卡组子集字库级联桩（大屏核心闭环阶段）
 *
 * 小屏 v1.4 T4.5：卡组专有生僻字按卡组差集生成子集 bin 拷入
 * SD /sdcard/fonts/deck_<id>.bin，cjk_text 主集 miss → 子集级联
 * 命中。大屏无 SD 卡，桩 lookup 恒 miss —— cjk_text 回退主集
 * 未收录画 cell 空心框占位（原路径语义）。
 *
 * 替换点：SD/卡组阶段以完整版覆盖本头 + app_stubs.c 删实现。
 */
#ifndef INKWORD_CJK_FONT_SD_H
#define INKWORD_CJK_FONT_SD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 桩：恒返回 NULL（未装载，级联回退占位框）。
 * 语义见小屏版：UTF-32 码点 -> 指定级字形位图，未收录 NULL。
 */
const uint8_t *cjk_font_sd_lookup_level(uint32_t cp, int level);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_CJK_FONT_SD_H */
