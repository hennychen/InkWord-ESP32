# 字体配置（P1+P2）设计与实施记录

> 2026-08-27 规划获批，2026-08-28 实施完成并上机（wft0290 2.9" 真机验证烧录）。
> 范围：P1a 字号三档化 / P1b 单词字号偏好 / P2 英文粗细切换。
> P3（CJK 运行期膨胀粗体）、P4（SD 家族字库）、32px 第四级（Flash 红线）经评估排除。

## 1. 设计原则

- **意图相对档位表达**：设置存用户意图（0/1/2），渲染层按布局档位（LAYOUT_TINY/SMALL/MID/LARGE）解释并钳位——与 `set_rot` 屏幕方向同一哲学，跨面板语义不漂移
- **默认零变化**：三个键缺省值均等价现状渲染（set_font=0 / set_word=0 / set_bold=0），出厂视觉零变化铁律
- **NVS 零迁移 + 回滚安全**：set_font 旧值 0/1 语义不变仅追加 2；旧固件读新值自然降级（2→"大字"），新固件读旧键名无冲突
- **惰性缓存范式**：新设置项沿用 settings_ui.c 的 static int8_t 缓存（-1 未载→NVS 首读），渲染热路径零 flash 读

## 2. 核心决策

| 决策 | 内容 | 理由 |
|------|------|------|
| 字号三档映射 | TINY/SMALL：{0→16px, 1→20px, 2→**1 钳位**}；MID/LARGE：{0→20px, 1→2→24px} | TINY 屏宽 122~128px 下 24px 每行仅 3~4 字；SMALL 横屏 176 高 24px 行距正文行数趋零 |
| reader 零逻辑改动 | `font_level_restore` 现有 +1 钳位对返回值 1/2 天然同效，仅更新注释 | 逻辑天然兼容三档语义 |
| 单词大小独立键 set_word | 值 [大\|中\|小] → ui_fit_font 起步档 [4\|3\2]，超宽自动降级不变 | 与 set_font 解耦（正文/单词独立调节是真实需求）；默认"大"=现状 start 4 |
| Bold 注入式 API | `epd_gfx_set_bold(bool)`：settings setter 即时 apply + main 初始化同步（音量 setter 同范式） | epd_driver 为底层不反向依赖 settings_ui；draw_text/text_bounds 每次调用 setFont，换表即时生效且量测/绘制一致 |
| 粗细影响范围 | 仅 FreeSans 路径（单词/状态栏数字/菜单 ASCII）；CJK 点阵与 IPA 音标不受影响 | 两套渲染路径物理隔离（点阵 vs GFX 字库） |
| 设置页行项 | 「单词大小」「粗细」插「字号」后，四处同步（SET_ITEMS 宏/k_full/k_tiny/row_value/on_button） | 滚动窗口机制支持任意行数；wft0290 自第 10 行由全显转滚动 |

## 3. 实施改动

| 文件 | 改动 |
|------|------|
| `src/settings_ui.c` | settings_font_mode 三档钳位；settings_word_size / settings_bold_enabled 新增；SET_ITEMS 8→10 + 两行项插入；粗细切换即时 epd_gfx_set_bold |
| `src/settings_ui.h` | NVS 键表追加 set_word/set_bold；三个 API 声明与注释 |
| `src/main.cpp` | UI_MEAN_LEVEL 三档映射（TINY/SMALL 钳位 20px）；UI_BODY_LINES 下限 1 防御；ui_word_start_size() + 三处单词渲染点参数化（测验 TF/T1 题干、学习页头部；Speak now/AI Chat 提示点排除）；初始化序列 epd_gfx_set_bold 同步 |
| `src/epd_driver.cpp` | Bold 双表（FreeSansBold 9/12/18/24pt）+ epd_gfx_set_bold + font_for_size 选表 |
| `src/epd_driver.h` | epd_gfx_set_bold 声明（注明仅 ASCII 路径生效） |
| `src/reader_engine.c` | 仅注释（+1 逻辑天然兼容） |
| `InkWord_Firmware/README.md` | 新增「设置页」小节（10 行项 + 字体三项表格）；音量行号引用 8→10 修正 |

### 计划偏差

- **14pt Bold 降用 12pt**：计划写 FreeSansBold14pt7b，实施验证 GFX 库仅 9/12/18/24pt 档（Arial14pt7b 同理需 fontconvert 本地生成）。Bold 表 14pt 槽改用 FreeSansBold12pt7b，后续可用官方 fontconvert 生成补齐。

## 4. 存量 bug 顺带修复：设置页反选框与文字偏移

**现象**（wft0290 真机 2026-08-28 反馈）：设置页菜单选项文字与选中黑色框错位——选中行白字大部分落在框外白底上不可见，邻行墨迹与框重叠。

**根因**：settings_ui.c `draw_row` 的 `base = y + lh*3/4` 是 **FreeSans 基线居中公式**，误喂给 `cjk_text_draw`——后者坐标语义为**字形 cell 顶边**（cjk_text.h 明确契约"基线语义不适用于本模块"）。字被压低 14px（TINY lh=28）/17px（MID lh=44）。

存量代码（本次改动前已存在）；RST 直达设置页（2026-08-27 上线）+ TINY 进 10 行滚动分支后被细看暴露。

**修复**：顶语义居中 `y + (lh - cell高)/2`（TINY y+6 / MID y+12）。

**同源排查**：menu_ui.c 与 wifi_config_ui.c 的 `*3/4` 均传给 epd_gfx_draw_text（FreeSans 基线 API），用法正确不受影响。教训入档：**cjk 点阵（顶语义）与 FreeSans（基线语义）两套 API 混用时，垂直居中公式不可互套**。

## 5. 真机过程记录（2026-08-28）

- **烧录错配**：先误烧 inkword-s3（3.7" MID 固件）到 2.9" 设备——满屏横屏线条乱纹、文字不可辨；串口 EPD-DIAG 仍报 COG ALIVE（串口正常不代表屏配正确，以日志 panel 行为准）。改烧 inkword-s3-wft0290 恢复。纪律：多台设备共用串口路径，烧录前必须确认当前接入设备型号。
- **烧录喇叭噪音**：烧录时（约 35s）喇叭较大噪音、完成后消失。定性为 ES8311+NS4150B 模块固有行为：NS4150B CTRL 脚板载 R10 上拉常开无 MCU 控制线 + 模块 USB 5V 独立供电，下载模式期间固件不运行，flash 写入纹波被常开模拟功放放大。日常 RST 复位不响（电流平稳）。用户决策接受现状；未来根治方案备档：挪 R10→R11 焊盘 + 飞线空闲 GPIO→CTRL + 固件控制。

## 6. 验证结果

- **构建矩阵**：inkword-s3 / inkword-s3-wft0290 / inkword-s3-demo 三环境 SUCCESS；**新增代码零警告**（全量重编显现的 1911 条存量警告——`%lu` format + ARDUINO_USB_MODE 重定义——逐行甄别均在改动区之外，系此前 touch 空转验证漏计，SCons 按内容 hash 判定不重编）
- **Flash**：bin 2,533,701B / 3MiB = 80.5%（Bold 四表实际 +19.2KB，远低于 80KB 预估），OTA 双分区安全线内
- **启动**：wft0290 真机 boot 全链正常（panel wft0290_bw 128x296、词库 ready、无 panic/重启循环）

## 7. 真机验证清单

- [x] 烧录启动正常（wft0290）
- [ ] 设置页 10 行滚动窗口 + 右上角 n/10 位置指示（wft0290 由全显转滚动）
- [ ] 字号三档循环：特大档 TINY/SMALL 渲染等价大字（正文 20px 钳位）；MID 大字/特大 24px 重排
- [ ] 单词大小三档：学习页头部/测验题干起步 24/18/14pt，长词降级不越界
- [ ] 粗细切换即时生效：英文/数字变粗，CJK 释义与 IPA 音标不变；14pt 档加粗降 12pt 目检
- [ ] 重启保持（NVS 恢复链）；全默认视觉与旧版一致
