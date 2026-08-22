/**
 * @file epd_panel.c
 * @brief L2 面板注册表 —— epd_panel_get_by_id() 查表实现
 *
 * 注册表为静态数组显式拼装（PANEL_COMPAT_DESIGN §5.3「链接期拼装静态
 * 注册表」的工程化落点）：未采用链接器 section 自动收集 —— ESP-IDF
 * 默认链接脚本无对应 KEEP 规则，--gc-sections 会回收无根引用的注册指针。
 * 新增面板三步：
 *   1. panels/xxx.cpp 定义 const epd_panel_desc_t（含 ops）；
 *   2. 此处 extern 声明；
 *   3. s_registry[] 追加一行。
 * Phase 3 构建矩阵落地：面板单元当前均无条件编入（S3 16MB Flash
 * 充裕，注册表完整保留运行期查表能力 —— NVS 运行期选屏预留）；
 * INKWORD_PANEL_* 宏只切换 EPD_PANEL_DEFAULT_ID（epd_panel.h），
 * 未选单元的编译期排除留作 Flash 紧张时的优化项
 */
#include "epd_panel.h"

#include <string.h>

/* panels/panel_depg0370_uc8253.cpp（3.7" BW，UC8253） */
extern const epd_panel_desc_t g_panel_depg0370;
/* panels/panel_e042a13_ssd1619.cpp（4.2" BWR 三色，SSD1619 手写序列，
 * 2026-08-22 真机勘误：初判 IL0398 有误，IC 实为 SSD1619） */
extern const epd_panel_desc_t g_panel_e042a13;
/* panels/panel_wf0270_ssd1680.cpp（2.7" BWR 三色，SSD1680 手写序列，
 * 22Pin 0.5mm FPC，维峰 WEIFENG WF0270T1PCZ2200E4；到货前未经真机） */
extern const epd_panel_desc_t g_panel_wf0270;
/* panels/panel_gdew027c44_il91874.cpp（2.7" BWR 三色，IL91874 手写序列，
 * 24Pin；2026-08-22 真机 bring-up 完成：原判 WF0270 型号有误，实物
 * IL91874/EK79652 家族，GxEPD2_270c 序列点亮，全刷 14720ms） */
extern const epd_panel_desc_t g_panel_gdew027c44;

static const epd_panel_desc_t *const s_registry[] = {
    &g_panel_depg0370,
    &g_panel_e042a13,
    &g_panel_wf0270,
    &g_panel_gdew027c44,
};

const epd_panel_desc_t *epd_panel_get_by_id(const char *id)
{
    if (!id) return NULL;
    for (size_t i = 0; i < sizeof(s_registry) / sizeof(s_registry[0]); i++) {
        if (strcmp(s_registry[i]->name, id) == 0) {
            return s_registry[i];
        }
    }
    return NULL;
}
