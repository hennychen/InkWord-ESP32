/**
 * @file cjk_font_sd.h
 * @brief SD 卡组子集字库级联查找（v1.4 T4.5 字库子集下发）
 *
 * 主集 cjk_font_data.bin 编入固件（GB2312 一级 + IPA + 标点 + ASCII），
 * 卡组专有生僻字（古诗文中的人名/地名用字等）按卡组差集生成子集 bin：
 *   后端 GET /api/admin/decks/<code>/charset 导出字符集
 *   → swift tools/gen_cjk_font.swift --subset <charset.txt> <deck_id>
 *   → ./deck_<id>.bin 拷入 SD /sdcard/fonts/deck_<id>.bin
 * 固件装载后 cjk_text 主集 miss → 子集级联命中（cjk_text.c adv_one）。
 *
 * bin 格式与主集 CKF1 同构（小端，头自描述）：
 *   [0..3]"CKF1" [4..5]ver [6..7]levels [8..11]u32 n
 *   [12..]cell[levels] 紧接 stride[levels]（cp 表起点 = 12+levels*4）
 *   cp 表 u16[n] 升序（4 对齐后）按级位图 n*(32/60/72/128)B
 * 几何与主集三方同源（gen_cjk_font.swift LEVELS / cjk_font.c 消费），
 * 装载时校验，不符拒载——级联命中的位图直接按主集几何 blit。
 * 级数兼容（2026-09-03 四级化）：levels ∈ {3,4} 均收——主集已升
 * 16/20/24/32px 四级，SD 旧三级子集（升级前生成）继续可用，
 * 新子集由升级后 swift --subset 生成（四级，含 32px 位图）。
 */
#ifndef INKWORD_CJK_FONT_SD_H
#define INKWORD_CJK_FONT_SD_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 装载活跃卡组的 SD 子集字库（/sdcard/fonts/deck_<deck_id>.bin）。
 *        先卸载已装载子集（切卡组语义）；文件不存在视作正常路径
 *        （主集已覆盖，仅清空当前子集）返回 1。
 * @param deck_id 卡组短 id（""=默认卡组，等价卸载，deck_<空> 不存在）。
 * @return 0 成功；1 文件不存在（子集清空）；-1 失败（头校验/尺寸/内存）。
 */
int cjk_font_sd_load(const char *deck_id);

/**
 * @brief 按绝对路径装载子集 bin（cjk_font_sd_load 的路径注入版，
 *        native 测试直吃 fixture 文件用；语义同上）。
 */
int cjk_font_sd_load_file(const char *path);

/** 卸载当前子集（释放 PSRAM；未装载时安全空操作） */
void cjk_font_sd_unload(void);

/** 是否已装载子集（级联查找前置快速判据） */
int cjk_font_sd_loaded(void);

/**
 * @brief 子集级联查找：UTF-32 码点 -> 指定级字形位图。
 *        未装载 / 越界级 / 未收录返回 NULL（调用方回退画占位框）。
 *        返回位图几何与主集同构（cell/stride 见文件头）。
 */
const uint8_t *cjk_font_sd_lookup_level(uint32_t cp, int level);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_CJK_FONT_SD_H */
