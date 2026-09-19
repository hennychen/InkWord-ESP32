# 大屏 UI 重设计方案（2026-09-17 落地版）

> 10.1" 1920×1080 ESP32-S3 单色墨水屏（ES108FC1C1）· InkWord_Firmware_BigScreen
>
> 全网调研（TRMNL 1-bit dashboard / 喵喵机单词卡 / E-ink Vocabulary Card E2·E3 /
> reMarkable·Kobo·Boox 纸感 / ACM 电子纸设计系统 / InkyPi 社区）提炼的 1bpp
> 设计法则落地：静态即完成态、层级五件套、反馈预算 <1s、焦点反白条、三区范式、
> 大字号红利、抖动仅图片。设计方向 = **词卡词典双栏 + 待机学习仪表盘**（A+B 组合）。

## 1. 设计规范

### 1.1 三区范式（全页面统一）

| 区带 | y 范围 | 底色 | 内容 |
|---|---|---|---|
| 顶栏 | 0 ~ 120 | 黑底白字 | 页标题/模式名 + 徽标 + 序号（词卡）|
| 主区 | 120 ~ 1000 | 白底（词卡左栏例外）| 页面主体 |
| 底栏 | 1000 ~ 1080 | 白底黑字 | 静态键位提示带（20px 点阵居中）|

底栏文案**静态**（不随词/态变化）→ 永不进局刷窗口，DU 差异行跳过天然免驱。

### 1.2 色彩纪律

- 全部经 `EPD_GFX_WHITE`(0) / `EPD_GFX_BLACK`(1) 宏，禁止 0/1 字面量
  （旧 menu_ui/settings_ui 的字面量与注释语义相反问题一并清除）
- 亮色词典风为基准（白底黑字）；黑底仅用于：顶栏、焦点反白条、词卡左栏、遮蔽块
- menu/settings 由暗色底改亮色是**有意变更**（统一词典风），非回归

### 1.3 字阶体系

**CJK 点阵六级**（cjk_font_data.bin，CKF1 自描述头运行时解析）：

| level | 0 | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|---|
| px | 16 | 20 | 24 | 32 | 40 | 48 |
| 用途 | — | 顶栏/提示/徽标 | 组头/小标 | 释义/进度/书名 | 音标/统计大字 | 引文/正文大字 |

字体链（gen_cjk_font.swift）：16/20px PingFang SC Bold 黑体、≥24px Kaiti SC
Bold 楷体；GB2312 一级 3755 字 + 全角标点 + ASCII + IPA 21 字符 + 引文/音标
收集集，n=3935 字形 × 六级 = 3,069,300B 位图（bin 总 3,077,208B）。

**英文/数字六档**（epd_gfx font_size，Adafruit GFX 格式）：

| font_size | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|
| 字表 | FreeSans 9pt | 12pt | 18pt | 24pt | **Helv 36pt** | **Helv 48pt** |

- Helv 四表由 `tools/gen_gfx_font.py` 从系统 Helvetica.ttc 生成（freetype-py，
  MONO 1bpp，**dpi=141** 官方口径，--check 校准 53 字形 maxdiff≤1px）
- 位图口径（epd_gfx.c drawChar 权威）：`bitmapOffset` 为**字节偏移**、字形起点
  整字节对齐、字形内连续位流 MSB-first 跨行累计——生成器 `flush_to_byte()` 兑现
- 表尺寸：36pt 17,056B(R)/18,606B(B) yAdv=71；48pt 30,569B(R)/32,756B(B)
  yAdv=94；`s_fonts[2][6]` 分派，`set_bold` 切换列
- 词头自适应降档：48pt 起测 `text_bounds`，超左栏宽限 480px 逐档降 36→24pt

### 1.4 刷新分档

| 档 | 触发 | 调用 | 体感 |
|---|---|---|---|
| GC16 全刷 | 进页/模式切换/换书/字号切换 | `epd_gfx_flush()` | ~3.3s |
| DU 全屏窗口 | 词卡翻词/收藏；阅读翻页/书签 | `flush_window(0,0,1920,1080)` | ~0.5s |
| DU 局部窗口 | 词卡揭晓/释义翻页（右栏）；菜单/设置光标/值变更（列表区） | 见 §2 | ~0.5s 级 |

gfx 层 K=8 次局刷自动插全屏重置驱白灰染（run91 纪律）；DU 差异行跳过——重绘
未变内容不驱屏。

## 2. 各页参数表

### 2.1 词卡（word_card_ui.c · 词典双栏）

```
┌───────────────────────────────────────────────────────────── 120
│ 顶栏黑底白字：模式名(20px) · 墨封/★徽标 · 序号(FreeSans 12pt)
├──────────────┬────────────────────────────────────────────── 
│ 左栏黑底白字  │ 右栏白底黑字 x∈[720,1840] w=1120
│ x∈[0,640]    │ 释义 level 4(48px) 行距 76 · 9 行/页
│ 词头 48pt B  │ 首行 y=218（主区垂直居中）
│ 基线 y=400   │ 遮蔽=黑块白字提示
│ 音标 40px    │ 页码指示右下 y=956
│ y=480        │
│ 徽标行 y=900 │
├──────────────┴────────────────────────────────────────────── 1000
│ 底栏：上下 释义/翻词 · SET 揭晓 · 左右 忘了/简单 · 长按中键 菜单
└───────────────────────────────────────────────────────────── 1080
```

刷新三档：首帧/切模式全刷；翻词/收藏变化（`s_last_collected` 跟踪）全屏窗口
DU；同词揭晓/释义翻页**右栏窗口** `flush_window(720,120,1120,880)`——左栏词头
不动是双栏布局的局刷红利。

### 2.2 菜单（menu_ui.c）

- 顶栏：「功能菜单」24px + 模式徽标（右）黑底白字
- 主区：组头行 70px（24px）+ 菜单项 140px（标签 32px 点阵 + 徽标全点阵右对齐）；
  焦点条黑底白字（PAD=80 收口全宽）
- 3 组头 + 7 项 = 1190px > 880 → **s_off 滚动窗口实装**（`ensure_visible` 贪心：
  sel 越上界直接作窗口首行，越下界逐行下移；`row_y` O(n) 线性定位）
- 光标移动 = 新旧两行重绘 + 列表区窗口 DU（滚动时整列表区重绘）
- 底栏：「上下 选择 · 中键 确认 · SET 返回」

### 2.3 设置（settings_ui.c）

- 4 行 × 140px 垂直居中（y0 = 120+(880-560)/2 = 280）；标签与值统一 32px 点阵
  （中文值：开/关、标准/大字/特大；FreeSans 退出本页）
- 音量行十格量程条（底线 + 实心格 48×28）+ 数字值
- 值变更/光标移动 = 行重绘 + 列表区窗口 DU；NVS 键不变（set_audio/set_vol/set_font）

### 2.4 待机仪表盘（standby_page.c）

- 上带 y140：「新词 N · 复习 M」40px（左）+「连续 X 天」40px（右）；y240 细线
- 中央：引文 48px 楷体（逐时轮换 esp_timer 降级语义保留）行距 84 居中；出处
  跟随引文块右下署名 20px
- 下带 y740 细线：进度条（x 160~1760，2px 外框，已学 = 词库 − 未学未墨封）
  +「已学 N / M 词」32px（左）+「墨封·收藏·错词」32px（右）
- 全刷 GC16（5 分钟待机进入才渲染，低频页）

### 2.5 阅读页（reader_page.c / reader_engine.c）

- 正文五档 20/24/32/40/48px（`RD_LEVEL_MAX` 3→5），XLARGE 默认 level 4（40px，
  layout_profile 表值 3→4）
- 顶栏书名·章节 24→32px；`RD_BOTTOM_BAR` 60→80 对齐三区；底栏中文键位条
- 翻页/书签 = 全屏窗口 DU（原「待 waveform_scanq 标定」注释作废——词卡 run88
  局刷已验证）；进页/字号/间距切换仍全刷
- 长按 SET 字号循环五档（NVS rd_font 持久化，旧值 1~3 兼容）

## 3. 字体资产管线

```bash
# CJK 六级（词库/引文/音标字符集 → bin + quotes_app.c/h 由 swift 生成）
cd InkWord_Firmware_BigScreen && swift tools/gen_cjk_font.swift
#   产物 src/app/data/cjk_font_data.bin（3,077,208B）+ quotes_app.*
#   头校验：levels=6 cells=(16,20,24,32,40,48) strides=(2,3,3,4,5,6) n=3935

# 英文大字表（系统 Helvetica.ttc → Adafruit GFX 格式 .h）
python3 tools/gen_gfx_font.py /System/Library/Fonts/Helvetica.ttc 0 36 src/gfx/fonts/Helv36pt7b.h
python3 tools/gen_gfx_font.py /System/Library/Fonts/Helvetica.ttc 1 36 src/gfx/fonts/HelvBold36pt7b.h
python3 tools/gen_gfx_font.py /System/Library/Fonts/Helvetica.ttc 0 48 src/gfx/fonts/Helv48pt7b.h
python3 tools/gen_gfx_font.py /System/Library/Fonts/Helvetica.ttc 1 48 src/gfx/fonts/HelvBold48pt7b.h
#   校准：--check 对官方 FreeSans24pt7b（dpi=141 同口径 maxdiff≤1px）
#   注意：Glyphs 数组末项必须 `};` 收尾（曾漏分号致全表 undeclared）
```

flash 预算：cjk bin 2.93MB（EMBED 至 factory 分区）+ Helv 四表 ~100KB + 固件
< factory 6MB，余量充足。

## 4. 验收清单（真机执行）

- [ ] 词卡：双栏视觉（黑底左栏白字词头 48pt）、长词降档观感、揭晓/释义翻页仅
      右栏闪动（~0.5s）、翻词全屏 DU、连续 8 次局刷后自动全屏重置无残影
- [ ] 菜单：亮色底 + 黑底焦点条、10 项滚动（最后一项可见）、光标移动 ~0.5s、
      徽标（计数/页数/无书）正确
- [ ] 设置：音量条步进、值变更局部闪动、NVS 断电保持
- [ ] 待机：今日统计/连续天数/进度条比例与 learning_state 一致、引文 48px 楷体
      居中、5 分钟自动进入 + 任意键退出
- [ ] 阅读：40px 正文可读性、五档字号循环、翻页 ~0.5s DU 无残影、书签星标
      局部更新、底栏中文提示
- [ ] 混排：词头 FreeSans/Helv 与音标点阵 IPA 同页视觉协调（Helvetica 同源风格）
- [ ] 回归：LAN 门户/阅读菜单/空态页/墨封流程不破

## 5. 关键决策记录

| 决策 | 理由 |
|---|---|
| 菜单暗色→亮色 | 词典风统一；焦点反白条在白底上更醒目 |
| 词头用 Helv 大字表而非点阵 | ASCII 点阵无 Bold、风格与 FreeSans 连续；48pt 表仅 31KB |
| cjk 六级全量不做子集 | 3.07MB flash 装得下；子集模式留作后续瘦身手段 |
| 阅读翻页复用全屏窗口 DU | run88 已验证；正文差异行自然稀疏，无需细粒度窗口 |
| 底栏静态文案 | 永不进局刷窗口，DU 差异跳过免驱；状态相关提示由页面主体承担 |
| 词卡收藏★走档 1（全屏窗口） | ★ 在顶栏，右栏窗口刷不到；`s_last_collected` 变化即升档 |
