# E042A13 三色屏局部刷新攻关终局报告

> 2026-09-01 ~ 09-02，A1→A10h 共 16 轮系统性尝试，终局结论：**硬件不支持**。
> 代码已回退，`panel_partial` 退化为全刷（14.7s/页）。

## 1. 背景与目标

Hink E042A13-A0（SSD1619 4.2寸 400×300 三色屏）当前全刷耗时 14.7s/页。
兄弟屏 E042A13-BW（黑白版）通过寄存器 LUT 差分局刷实现 ~1.3s/页（见 `E042A13BW_PARTIAL_REFRESH.md`）。
目标：为三色屏实现类似的快速局刷。

## 2. 攻关历程（A1→A10h 共 16 轮）

### Phase 1：Mode 1 格式 LUT 尝试（A1-A9，9 轮）

移植黑白兄弟屏的 Mode 1 LUT 格式（BB/BW/WB/WW 通道），走 0x22/0xEC 局刷序列。

| 轮次 | 改动 | BUSY 时长 | 显示效果 | 结论 |
|------|------|-----------|----------|------|
| A1 | 0x21 单参数 | 20s 超时 | 无变化 | 证伪 |
| A2-A7 | 各种 LUT/序列组合 | 35s 超时 | 全白/无变化 | Mode 1 LUT 格式不被消费 |
| A8 | 0x22/0xEC | 35s 超时 | 全白 | BUSY 不释放 |
| A9 | 0x22/0xFC（全位组合） | 35s×2 超时 | 全白 | 硬件锁死实锤 |

**Phase 1 结论**：Mode 1 格式 LUT 在此屏完全无效，BUSY 永不释放或超时。

### Phase 2：Mode 2 差分格式尝试（A10-A10h，7 轮）

全网搜索发现仁波切博客：SSD1677 10.2寸三色屏通过 Mode 2（黑白差分模式）实现 ~1s 局刷。
核心发现：Mode 1（三色 BWR）和 Mode 2（黑白 BW）的 LUT 格式、RAM 语义、0x22 位表完全不同。

| 轮次 | 改动 | BUSY 时长 | 显示效果 | 结论 |
|------|------|-----------|----------|------|
| A10 | Mode 2 LUT + 单次激活 | 14.7s | 全白 | LUT 未生效，跑 OTP 波形 |
| A10b | init 去 0xCF，局刷动态设 | 14.7s | 红色背景+白黑重叠 | 0xCF 污染全刷 |
| A10c | 0x22/0xDF（含 0x10 Load LUT） | 12s | 浅红叠加 | 0x10 位语义错误 |
| A10d | init 加 0xCF，局刷 0xCF | 12s | 浅红叠加 | Mode 2 仍未激活 |
| A10e | 加 0x37（7 字节，F=0x40） | 12s | 浅红叠加 | 0x37 值不对 |
| A10f | 0x37 改 10 字节 + 0x99 | 10.8s | 浅红叠加 | SSD1619 不接受 10 字节 |
| A10g | 0x37 回 7 字节，F=0x4F | 10.8s | 浅红叠加 | Mode 2 仍未激活 |
| A10h | （未烧录，结论已明确） | — | — | 终止 |

**Phase 2 症状**："浅红色叠加" = 引擎仍在 Mode 1 三色模式下解释 0x26（Red RAM）和 0x24（B/W RAM），
Mode 2 从未真正激活。BUSY 始终 10-14s = 全刷时间，自定义 LUT 从未被消费。

## 3. 终局结论

### 3.1 硬件限制确认

**Hink E042A13-A0 三色屏硬件不支持局部刷新。**

交叉验证来源：
1. **GxEPD2 作者（ZinggJM）**：明确标注 4.2" 3-color 不支持 partial update
2. **Arduino Forum**："The Black/White/Red 4.2" doesn't support partial refresh. Only the Black/White version (GDEW042T2) does."
3. **中文社区**："三色屏是不支持局部刷新的"
4. **仁波切博客**：其 Mode 2 方案针对 SSD1677 ESL 价签屏（OTP/膜组完全不同），不可移植到 SSD1619

### 3.2 根因分析

1. **OTP 无差分波形**：SSD1619 三色屏 OTP 中未预载局刷波形，寄存器 LUT 路径被硬件屏蔽
2. **Mode 2 不可激活**：无论 0x37/0x22 如何组合，引擎始终在 Mode 1 三色模式下运行
3. **兄弟屏差异**：黑白版（E042A13-BW）的 OTP 预载了差分波形，三色版没有

### 3.3 处理方案

`panel_partial` 退化为全刷（`do_refresh`），`partial_enabled` 保持 `true` 以兼容上层调度逻辑，
但无速度收益。每页刷新仍需 14.7s。

## 4. 技术收获

尽管局刷未成功，本轮攻关沉淀了关键认知：

1. **0x22 位表完整破译**：
   - 0x80=Enable Clock, 0x40=Enable Analog, 0x20=Load Temperature, 0x10=Load LUT
   - 0x08=Mode2（0=Mode1）, 0x04=Enable DISPLAY, 0x02=Disable ANALOG, 0x01=Disable OSC
   - 0xC7=CLK+ANALOG+Mode1, 0xCF=CLK+ANALOG+Mode2, 0xF7=C7+0x30(LoadTemp+LoadLUT)

2. **Mode 1 vs Mode 2 格式差异**：
   - Mode 1（三色 BWR）：LUT 格式 BB/BW/WB/WW，RAM 0x24=BW/0x26=Red
   - Mode 2（黑白 BW）：LUT 格式 B2B/B2W/W2B/W2W（新旧状态转换），RAM 0x24=NEW/0x26=OLD

3. **BUSY 释放必要条件**：0x10（Load LUT）步骤必须执行，否则 BUSY 永不降

4. **0x37 Display Option**：控制 WS(LUT) 不同时间片对应的 Display Mode + RAM Ping-Pong（F[6]）

## 5. 后续建议

| 方案 | 耗时/页 | 效果 | 成本 |
|------|---------|------|------|
| A. 接受全刷 14.7s | 14.7s | 当前可用，清晰 | 无 |
| B. 换黑白屏（GDEW042T2） | ~2s 局刷 | 需换硬件 | 屏幕成本 ~50 元 |
| C. 优化全刷体验 | 14.7s | 闪屏减少/声音提示 | 软件工作量 |

**当前选择**：方案 A（接受全刷 14.7s），代码已回退稳定。

## 6. 代码变更摘要

- `panel_e042a13_ssd1619.cpp`：
  - 删除 `panel_init` 末尾的 0x37/0x22 Mode 2 代码
  - 删除 `k_lut_mode2_bw` LUT 数组和 `set_full_window` 函数
  - `panel_partial` 退化为全刷（`do_refresh`）
  - `desc.partial_ms` 改为 15000（= full_ms）
  - 注释记录终局结论和交叉验证来源

---

**文档维护**：若后续更换黑白屏硬件，可重新启用局刷（参考 `panel_e042a13bw.cpp` v8 定稿）。
