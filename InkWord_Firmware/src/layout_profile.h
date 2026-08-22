/**
 * @file layout_profile.h
 * @brief L4 布局档位层：运行期按 GFX 短边分档，提供档位参数
 *        （PANEL_COMPAT_DESIGN §8.1 / §11.2；Phase 5 交付物）
 *
 * 档位与面板解耦：同一档位覆盖多块屏（MID = 3.7" 416x240 与
 * 4.2" 400x300）。单面板编译期选定（INKWORD_PANEL_ID），gfx 尺寸
 * 运行期恒定，首次调用后缓存；调用方均在主 loop 上下文
 * （standby / reader / main），无并发。
 *
 * 字库级映射（§11.2 三级字号映射表；MID 列 = 现役 416x240 视觉基线）：
 *   quote_level —— 大字场景（待机引文出处 / 阅读占位提示）：
 *     SMALL=0(16px) / MID=2(24px) / LARGE=2(24px，32px 级生成后升 3)
 *   reader_level —— 阅读正文默认级（用户 NVS 字号优先，此处仅 miss 默认）：
 *     SMALL=0(16px) / MID=1(20px) / LARGE=2(24px)
 * 学习页释义恒 level 0（16px 为字库下限，全档适用，无映射必要）。
 */
#ifndef INKWORD_LAYOUT_PROFILE_H
#define INKWORD_LAYOUT_PROFILE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LAYOUT_SMALL = 0,   /**< 短边 <200px：2.7" 264x176 */
    LAYOUT_MID,         /**< 200~319px：3.7" 416x240 / 4.2" 400x300 */
    LAYOUT_LARGE,       /**< >=320px：7.5" 800x480 */
} layout_kind_t;

typedef struct {
    layout_kind_t kind;     /**< 档位（调试/日志用） */
    int quote_level;        /**< 大字场景字库级（引文/占位提示） */
    int reader_level;       /**< 阅读正文默认字库级（NVS miss 时） */
} layout_profile_t;

/** 按运行期 gfx 短边分档（首调缓存；须在 epd_driver_init 之后调用） */
const layout_profile_t *layout_profile_get(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_LAYOUT_PROFILE_H */
