# OPM021EB 显示质量优化与诊断报告

**日期**：2026-09-10 ~ 2026-09-12  
**面板**：OPM021EB（2.13" 122x250 BW，ESL 拆机屏，UC8151D 控制器）  
**结论**：白色驱动不足为 ESL 屏物理特性，软件补偿效果有限

---

## 1. 问题描述

用户反馈 OPM021EB 屏幕显示问题：
- 中间区域灰暗
- 字体不清晰（尤其释义/说明小字）
- 翻页闪屏三次

## 2. 诊断测试

### 2.1 屏幕老化诊断工具

创建 `screen_aging_test.c/h`，包含 6 项诊断测试：
- 全白均匀性测试
- 全黑均匀性测试
- 九宫格分区测试
- 打断时间扫描（6 档 500/750/1000/1500/2000/2970ms）
- 快速翻页残影测试
- 全刷 vs 打断法对比

通过 `INKWORD_SCREEN_AGING_TEST` 宏控制，默认关闭。

### 2.2 诊断结果

| 测试 | 结果 | 结论 |
|------|------|------|
| 全白均匀 | 中间和边缘基本一致，隐约一层灰 | 非老化，固有光学特性 |
| 全黑均匀 | 正常 | 黑色驱动无问题 |
| 九宫格 | 2/4/6/8（白底黑字）不清晰，1/3/5/7/9（黑底白字）清晰 | **白色驱动不足** |
| 打断时间扫描 | 所有档位白底都偏灰 | 物理特性，打断时间无法修复 |
| VCOM/CDI 扫描 | 无效 | OTP LUT 模式下寄存器被忽略 |

**根因**：ESL 拆机屏 OTP 波形中白色驱动相位能量不足，白底始终偏灰。

## 3. 修复与补偿

### 3.1 打断法修正（2026-09-10）

**问题**：DTM1 写 new_ 导致 DTM1=DTM2，COG 差分引擎无差异可驱。

**修复**：
- DTM1(0x10) 写 prev 旧帧（真差分）
- 打断时间 250ms → 500ms

**效果**：改善中间区域对比度，翻页 ~0.9s/帧。

### 3.2 喇叭启动噪声修复（2026-09-10）

**问题**：启动/烧录期间 I2S GPIO 6 悬空，被 ES8311 DAC 拾取 → 功放 → 喇叭"滋啦"。

**修复**：
- `es8311.c`：REG_DAC31 init 时设 0x60（soft mute），dac_start 再解除
- `main.cpp`：setup() 最早期对 I2S_DATA_OUT_PIN 启用内部下拉

### 3.3 软件补偿尝试（2026-09-10 ~ 09-12）

| 方案 | 效果 | 状态 |
|------|------|------|
| 粗体字体（FreeSansBold） | 仅 ASCII 生效，CJK 无效 | 关闭 |
| 像素膨胀 1px/2px | 白字光晕/糊连 | 关闭 |
| 帧反相（黑底白字） | 白字发灰/光晕 | 关闭 |
| 局刷 2 遍 | 改善有限 | 关闭 |
| 打断 750/1000ms | 改善有限 | 回退 500ms |

**结论**：ESL 屏白色粒子迁移精度差，软件补偿无法突破物理极限。

## 4. 新增基础设施

### 4.1 面板描述符扩展字段（`epd_panel.h`）

```c
bool text_bold;      /* 文本加粗（低对比度屏补偿） */
bool pixel_dilate;   /* 像素膨胀（帧级后处理） */
bool frame_invert;   /* 帧反相（黑底白字） */
```

### 4.2 帧处理管线（`epd_driver.cpp`）

```
canvas → transpose → [pixel_dilate] → [frame_invert] → panel frame
```

- `frame_dilate_cross()`：1px 十字膨胀（可扩展为 Npx）
- `frame_invert`：B/W 平面逐位取反

### 4.3 打断时间覆盖（`panel_opm021eb.cpp`）

```c
volatile uint16_t g_abort_ms_override = 0;  /* 0 = 使用默认 K_ABORT_MS */
```

供诊断测试运行时覆盖打断时间。

## 5. 当前配置（2026-09-12 恢复）

```c
.text_bold = false,
.pixel_dilate = false,
.frame_invert = false,
.passes = 1,
K_ABORT_MS = 500,
partial_enabled = true,
```

**白底黑字，打断法快刷 ~0.9s/帧。**

## 6. 文件清单

| 文件 | 变更 |
|------|------|
| `src/epd_panel.h` | +3 字段（text_bold/pixel_dilate/frame_invert） |
| `src/epd_driver.cpp` | +frame_dilate_cross()、+init 粗体自动应用、+canvas_to_panel 管线 |
| `src/panels/panel_opm021eb.cpp` | 打断法修正、+g_abort_ms_override、+desc 字段 |
| `src/es8311.c` | DAC soft mute 修复启动噪声 |
| `src/main.cpp` | I2S 引脚下拉、测试模式入口 |
| `src/screen_aging_test.c` | 新建，6 项诊断测试 |
| `src/screen_aging_test.h` | 新建，测试接口 |
| `src/CMakeLists.txt` | +screen_aging_test.c |
| `platformio.ini` | +INKWORD_SCREEN_AGING_TEST 宏（已注释） |
