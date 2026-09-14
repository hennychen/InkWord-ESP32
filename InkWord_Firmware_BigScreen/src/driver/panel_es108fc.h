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
