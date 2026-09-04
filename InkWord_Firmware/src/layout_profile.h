/**
 * @file layout_profile.h
 * @brief L4 布局档位层：运行期按 GFX 短边分档，提供档位参数
 *        （PANEL_COMPAT_DESIGN §8.1 / §11.2；Phase 5 交付物）
 *
 * 档位与面板解耦：同一档位覆盖多块屏（MID = 3.7" 416x240 与
 * 4.2" 400x300）。档位运行期按 gfx 短边自选（首次调用后缓存），gfx 尺寸
 * 运行期恒定，首次调用后缓存；调用方均在主 loop 上下文
 * （standby / reader / main），无并发。
 *
 * 字库级映射（§11.2 三级字号映射表；MID 列 = 现役 416x240 视觉基线）：
 *   quote_level —— 大字场景（待机引文出处 / 阅读占位提示）：
 *     TINY=0(16px)/ SMALL=2(24px，待机页紧排版配合)/ MID=2(24px) /
 *     LARGE=3(32px，2026-09-03 32px 字库级落地后升级)
 *   reader_level —— 阅读正文默认级（用户 NVS 字号优先，此处仅 miss 默认）：
 *     TINY=0(16px，122~128px 宽下 20px 每行仅 4~5 字)/ SMALL=1(20px) /
 *     MID=1(20px) / LARGE=3(32px)
 * 学习页释义恒 level 0（16px 为字库下限，全档适用，无映射必要）。
 *
 * 2026-08-23 新增 TINY 档（三块在途屏适配前置）：2.13" 122x250 /
 * 2.9" 128x296 电子标签屏竖屏形态（用户选型竖持），短边 122~128px
 * 下 SMALL 头部几何（状态栏 32+头部 72）正文区趋零，拆独立档。
 *
 * T1.5 几何参数表（2026）：散落 6 文件的 `XXX_TINY ? a : b` 三元
 * 布局宏收敛为档位字段（§8.1 兑现）；T1.6 追加 kb_scale（配网键盘
 * SMALL 缩放）、T1.7 追加 partial_* 局刷保养阈值页面系数（partial_menu
 * 为 2026-09-04 新增，非原样搬运，见字段注释）。取值 =
 * 迁移前三元宏**原样搬运**（视觉零变化铁律，
 * 416x240/264x176/122x250 三档真机基线）；
 * LARGE 档几何字段为预估值，「7.5" 上机校准」。字号/行距类派生
 * 宏（依赖运行时用户字号设置者）不表化，保留各文件派生式。
 */
#ifndef INKWORD_LAYOUT_PROFILE_H
#define INKWORD_LAYOUT_PROFILE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LAYOUT_TINY = 0,    /**< 短边 <140px：2.13" 122x250 / 2.9" 128x296 竖屏 */
    LAYOUT_SMALL,       /**< 140~199px：2.7" 264x176 */
    LAYOUT_MID,         /**< 200~319px：3.7" 416x240 / 4.2" 400x300 */
    LAYOUT_LARGE,       /**< >=320px：7.5" 800x480 */
} layout_kind_t;

typedef struct {
    layout_kind_t kind;     /**< 档位（调试/日志用） */
    int quote_level;        /**< 大字场景字库级（引文/占位提示） */
    int reader_level;       /**< 阅读正文默认字库级（NVS miss 时） */
    /* ---- T1.5 几何参数（三元宏取值原样搬运；字段注释标注源宏，
     * 消费方 main/menu_ui/quiz_ui/chat_ui/review_ui/standby_page）---- */
    int status_h;           /**< 状态栏高度：UI_STATUS_H / MU_TITLE_H */
    int margin_x;           /**< 左右留白：UI_MARGIN_X / MU_MARGIN_X */
    int body_reserve;       /**< 正文区底部预留：UI_BODY_RESERVE */
    int item_h;             /**< 列表行高：MU_ITEM_H / RV_ITEM_H */
    int hint_h;             /**< 菜单底部提示栏高（TINY 省略）：MU_HINT_H */
    int rv_hint_h;          /**< 复习词表底部提示行预留：RV_HINT_H */
    int font_lvl_main;      /**< 菜单主内容 cjk level：MU_FONT_LVL */
    int font_px_main;       /**< 菜单主内容字号 px：MU_FONT_H */
    int ascii_size_main;    /**< 菜单 ASCII 徽标 FreeSans size：MU_FONT_ASC */
    int info_lh;            /**< 菜单 INFO/按键说明行高：MU_INFO_LH */
    int tight_quote;        /**< 待机引文紧排版（TINY/SMALL=1）：原 s_tight */
    int narrow_tiny;        /**< TINY 档内 122 宽特判（OPM021EB 辅助字级
                              * 跟随正文；128 宽 WFT0290 保持 16px 原口径
                              * 2026-08-30 真机定稿）——width 类判断属
                              * 档位职责，非 TINY 恒 0；get() 运行期填 */
    int kb_scale;           /**< 配网键盘/列表缩放百分比（T1.6）：SMALL=60
                              * （2.7" 264 宽行 0 键盘 219px / 176 高键区
                              * 4×19+3×1=79px 双约束定档）；其余 100 =
                              * 416 基线原值（int 截断无损，视觉零变化） */
    int partial_std;        /**< 局刷保养阈值页面系数百分比（T1.7）：学习/
                              * 默认页 100；阈值统一公式 desc.partial_count
                              * _full_refresh × 系数（学习 100/待机 150/
                              * 配网 125，现值 8/12/10 反推，行为零变化） */
    int partial_standby;    /**< 待机页系数：150（8→12；无窗口双 RAM 局刷
                              * 无残影，真全刷降为例行保养，轮换下 ≈ 1h */
    int partial_wifi;       /**< 配网页系数：125（8→10，介于待机 12/学习 8） */
    int partial_menu;       /**< 菜单/列表选择页系数：400（2026-09-04 用户
                              * 反馈上下选择保养全刷闪烁打断；导航页短驻留、
                              * 光标移动仅小面积高亮条翻转残影累积慢，且
                              * 退出/换页全刷自然清理 → 阈值公式化调大：
                              * OPM/WFT 4×4=16、DEPG 8×4=32、E042BW
                              * 16×4=64，原硬编码 10 全屏一律 10） */
} layout_profile_t;

/** 按运行期 gfx 短边分档（首调缓存；须在 epd_driver_init 之后调用） */
const layout_profile_t *layout_profile_get(void);

/** 清档位缓存（native-test 专用：改桩 gfx 尺寸后重新分派） */
void layout_profile_test_reset(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_LAYOUT_PROFILE_H */
