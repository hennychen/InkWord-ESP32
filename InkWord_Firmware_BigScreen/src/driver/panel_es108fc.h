#pragma once
// ES108FC1C1-RHY 面板安全初始化与访问（方案 §3.3）
#include <stdbool.h>
#include "esp_err.h"
#include "epdiy.h"

// 安全初始化：PSRAM 检查 → epd_init（V7 板 + 保守参数）→ VCOM 范围
// 检查 → 全白清除验证。幂等：重复调用时先 deinit 再按当前
// display 参数（含外部修改的 bus_speed）重初始化，hl 帧缓冲复用
// 不重复分配（速率爬升场景防 PSRAM 泄漏）。
esp_err_t panel_es108fc_safe_init(void);

// 高层状态访问（帧缓冲获取/更新用，调用前须 safe_init 成功）
EpdiyHighlevelState* panel_es108fc_hl(void);

// 显示描述访问（速率爬升时改 bus_speed 后重新 safe_init）
EpdDisplay_t* panel_es108fc_display(void);

// 反初始化（深睡前安全断电路径）
void panel_es108fc_deinit(void);

// ---- 双路径电源控制（卖家板型实测后钉路径，2026-09-12） ----
// 卖家 epdiy2 魔改版已注释库内 I2C PMIC 全链路，配套 .ino 用 GPIO46
// 直控电源。bring-up 步骤 2 扫描 I2C 后调 set_pmic_online() 钉路径：
//   PMIC 在线 → 库 epd_poweron()/epd_poweroff()（真 PG 等待）；
//   PMIC 缺失 → 仅 GPIO46 使能/断电（卖家示例同构）。
// 两条路径 GPIO46 均拉高/拉低（无害双保险）。
void panel_power_on(void);
void panel_power_off(void);
void panel_es108fc_set_pmic_online(bool online);

// ---- 窗口局部刷新（run88 P4 路径，2026-09-16 复活） ----
// 卖家 patch 库的 epd_hl_update_area 已被魔改（diff_area 强制全屏、
// previously_white/black 强制 false，highlevel.c L134-140），局刷退化
// 为全屏扫描。真窗口局刷走底层直调：difference_image_cropped 取真实
// 差异窗口 → epd_draw_base（MODE_DU 5 相位，P4 实测 554ms 级）→ back
// 脏行同步（对齐库同款语义）。
// LCD 引擎每帧扫满 1920x1080，故 epd_draw_base 的 area 必须是全屏
// （render_lcd.c 断言 + difference_fb 全屏 stride，bring-up §10.1）；
// “窗口”由 no-drive 掩码达成：纵向 dirty_lines 先清零只留本窗脏行，
// 横向同行窗口外列填 0xFF（from==to 即 keep）。省的是驱动次数，
// 不是扫描时间。
// 前提：调用前 front_fb 已写入目标内容（与 hl 链路同构）。
// 灰染纪律（§10.3/run91）：nop 槽泵电荷随扫描累积，须每 K 次局刷
// 插入全屏 GC16 重置（epd_gfx 层计数）。
esp_err_t panel_es108fc_update_area(int x, int y, int w, int h);
