# 墨读 / InkRead PRD V2.0 × 代码库差距分析

> **依据**：PRD V2.0（2026-08-18，需求评审稿）× 仓库代码实测（2026-08-19 全量核对）。
> **权威基线**：固件行为以 [gpio_config.h](../InkWord_Firmware/src/gpio_config.h)、
> [main.cpp](../InkWord_Firmware/src/main.cpp) 头注释、[WIRING_DIAGRAM.md](WIRING_DIAGRAM.md) 为准；
> 云端以 [InkWord_Backend](../InkWord_Backend/src) / [InkWord_Admin](../InkWord_Admin/src) 实体与控制器为准。
> **维护原则**：本文档随 PRD 版本与固件演进同步更新；结论冲突时以代码与接线实测为准。

---

## 一、总体结论

1. **项目实际进度超前于 PRD 里程碑叙述**：约处于 P3 后半（核心功能）完成、P4（云端后台）收尾，
   且已落地多个 PRD 未规划的功能（待机页引文轮换、AP/LAN 双通道配网直传、天气聚合、HTTP Date 校时）。
2. **PRD V2.0 四大新增需求（阅读模式、语音跟读、震动反馈、错词本/收藏）在代码中零实现**。
3. **PRD 4.2 GPIO 分配表与已验证接线全面不符，且含 strapping 硬伤**（GPIO3/45/46），
   照 PRD 施工会推翻大量已验证成果并引入启动级故障 —— **动工前必须先修订 PRD**。
4. **引脚资源已近枯竭**是 V2.0 硬件新增（麦克风/马达/蜂鸣器）的第一约束，详见 §6.3。

---

## 二、固件模块级对照

框架事实：**Arduino + PlatformIO**（espressif32@7.0.17 core 2.0.17，非 PRD 所述 ESP-IDF/EPDiy V7）。
显示栈：GxEPD2 1.6.9 定制类 [GxEPD2_374_DEPG0370](../InkWord_Firmware/src/GxEPD2_374_DEPG0370.h)
（UC8253 类 COG，240×416，全刷 1500ms / 局刷 350ms 实测标称）+ Adafruit GFX 1.12.6。

| PRD 模块 | 现状 | 说明 |
|:---|:---:|:---|
| 墨水屏驱动（全刷/局刷/清残影） | ✅ | demo 忠实版双平面局刷 + 硬复位；面板级 partial 残影缺陷已定性，以低阈值真全刷清洗（见驱动头注释） |
| 闪卡/听写/复习三模式 | ✅ | [study_mode_machine.h](../InkWord_Firmware/src/study_mode_machine.h)；SET 键遮蔽/揭晓自测 |
| **阅读模式**（TXT/4 级字号/翻页/进度） | ❌ | 全库无 reader/分页实现；阻塞点为 CJK 字库（见 §8.3） |
| **错词本 + 收藏** | ❌ | 无 ConsecutiveWrong/IsCollected 逻辑（固件与后端两侧皆无） |
| SM-2 引擎 | ✅ | [srs_engine.h](../InkWord_Firmware/src/srs_engine.h)：质量分 0~5、EF ∈ [1.3, 2.8] |
| 刷新调度器 | ✅⁺ | [refresh_scheduler.h](../InkWord_Firmware/src/refresh_scheduler.h)：阈值计数 + 分页面阈值 `_n` 版本，强于 PRD 伪代码；PRD 的"面积>30% 走全刷"规则未实现（由调用方显式指定窗口） |
| I2S 音频输出（MAX98357，WAV/MP3） | ✅ | [audio_player.h](../InkWord_Firmware/src/audio_player.h)；引脚 4/5/6 |
| **INMP441 麦克风 / 语音跟读评测** | ❌ | 零代码；PRD 亦未定义评测算法与验收标准 |
| **震动马达事件反馈** | ❌ | 仅引脚预留（GPIO41，见 gpio_config.h 注释），无驱动无事件表 |
| **蜂鸣器** | ❌ | 无代码；且 PRD 4.1 BOM 未列蜂鸣器元件（PRD 自相矛盾） |
| OTA（双分区 + MD5 + 回滚） | ✅ | [ota_manager.h](../InkWord_Firmware/src/ota_manager.h) |
| HTTP 同步（词库/进度/心跳） | ✅⁺ | [sync_client.h](../InkWord_Firmware/src/sync_client.h)：另含天气转发 + HTTP Date 校时（超出 PRD） |
| SD 卡 FATFS | ✅ | 独立 SPI3 总线（17/16/18/47），挂载点 `/sdcard`；非 PRD 的与屏共用总线方案 |
| 配网 | ✅⁺ | STA 配置 UI + AP Captive Portal + LAN 浏览器直传（[lan_display_server.cpp](../InkWord_Firmware/src/lan_display_server.cpp)）；BLE 配网代码已写但**默认禁用**（Arduino 预编译库无 Wi-Fi/BLE coex，真机崩溃循环，`INKWORD_BLE_PROVISION=0`） |
| 深度休眠 / 待机功耗 <5µA | ◐ | 仅屏幕 hibernate（0x07/0xA5）；SoC 级深睡未实现；电量读取为 TODO 硬编码 `bat=100`（main.cpp:367） |
| 待机页（词库为空时） | ✅⁺ | PRD 外功能：《传习录》引文 24px 子集楷体轮换（5 分钟/条，24 条），NVS 天气缓存 + 自治时钟 |
| 词库容量 | ⚠️ | `MAX_WORDS=64`（DRAM 限制），与"人教版全年级词库"目标相差数量级，需数组入 PSRAM 或 SD 分页加载 |

---

## 三、交互映射差异（PRD 5.3 vs main.cpp 实测）

当前按键语义基线（[main.cpp 头注释](../InkWord_Firmware/src/main.cpp) 与
[WIRING_DIAGRAM.md §2.3](WIRING_DIAGRAM.md)）：

| 键 | PRD 定义 | 当前实现 | 兼容性 |
|:---|:---|:---|:---|
| 上 | 上一词 | 短按=上一条 / 长按=清残影全刷 | ✅ 兼容 |
| 下 | 下一词/翻卡 | 短按=下一条 / 长按=**切换学习模式** | ✅ 兼容 |
| 中 | 播放读音 | 短按=发音 / 长按=Wi-Fi 配置 | ✅ 兼容 |
| 左 | 自评"忘记" | 短按预留 / 长按=AP 配网门户 | ⚠️ 自评语义未落地（现由 SET 遮蔽/揭晓承载） |
| 右 | 自评"简单" | 短按预留 / 长按=LAN 接收页 | ⚠️ 同上 |
| 长按中 | 收藏 | 无（已占用为配网） | ❌ 冲突 |
| SET 侧键（GPIO42） | **PRD 未定义** | 短按=翻义/轮换引文 / 长按预留 SRS"记得" | ❌ PRD 缺失 |
| RST 侧键（GPIO40） | **PRD 未定义** | 短按=回第一条 / 长按预留 SRS"忘了" | ❌ PRD 缺失 |

**结论**：五向 + SET/RST 共 7 键、长短按共 14 个语义槽位，容纳 PRD 5.3 全部映射绰绰有余；
但 PRD 5.3 把"收藏"绑在长按中键上与现行配网功能冲突，且完全遗漏 SET/RST 两个已接线实体键。
需要一次交互语义总表重排（建议随错词本/收藏功能一并定稿）。

---

## 四、云端对照（InkWord_Backend / InkWord_Admin）

架构与 PRD 6.1 完全一致（.NET 8 五层 + PostgreSQL + Redis + Hangfire + JWT + Docker/Nginx）。
API 端点基本齐平：`words/import` ≈ PRD `batch-import`，另多出 `auth/login`、`device/weather`。
管理端四模块（dashboard / device-management / ota / word-management）已就位。

**差距集中在数据模型（PRD 6.2 vs 实体）**：

| PRD 表 | 实际 | 差距 |
|:---|:---|:---|
| Users（Email/Nickname/**DailyNewWordGoal**） | [User.cs](../InkWord_Backend/src/InkWord.Core/Entities/User.cs) = **管理后台账号**（Username/Role） | ❌ 语义级差异：当前"设备即用户"，LearningRecord 挂 **DeviceId** 而非 UserId |
| Words | [Word.cs](../InkWord_Backend/src/InkWord.Core/Entities/Word.cs) | ❌ 缺 Root 词根、Inflections、Source/Grade 教材溯源；Tag 为单字符串非数组；Example 未拆双语 |
| LearningRecords | [LearningRecord.cs](../InkWord_Backend/src/InkWord.Core/Entities/LearningRecord.cs) | ❌ 缺 **ConsecutiveWrong、IsCollected** —— 错词本/收藏的后端依赖缺失 |
| Devices / OtaPackages | ✅ | 基本对齐（心跳字段、双分区元数据齐备） |

管理端"学习分析（SRS 分布、错词排行）"无独立模块，且错词数据源本身缺失。

---

## 五、Flutter App 范围缺口

PRD **完全未规划** InkWord_App，但项目已实际投入：
BLE 配网双侧架构（App 端 GATT 客户端 + 设备端 `ble_provision.cpp`）、
帧协议编解码（`test/frame_codec_test.dart`）、文本排版（`test/text_layout_test.dart`）。
注意：设备端 BLE 当前因 coex 问题禁用（见 §2），App 配网主链路实际走 **AP/LAN 直传**。
PRD 需增补"移动端"章节或明确 App 与 Captive Portal 的分工。

---

## 六、硬件红线：GPIO 对照与引脚资源核算 ⚠️（最高优先级）

### 6.1 PRD 4.2 分配表的致命错误

| PRD 分配 | 问题 |
|:---|:---|
| 摇杆含 **GPIO3** | JTAG strapping：按住该键复位/上电会把 JTAG 路由到 GPIO39-42，USB-JTAG 烧录失联（实测教训，见 WIRING_DIAGRAM §三注） |
| MAX98357 **BCLK=45 / LRC=46** | GPIO45/46 均为 strapping（上电采样须为低，外接上拉/强驱动可致无法启动）；**GPIO46 且为 input-only，根本无法输出 LRC** —— PRD 方案有硬伤 |
| SD 与屏共用 SPI（SCK14/MOSI13） | 与 GxEPD2 的 SPI2 事务模型有冲突风险；实测已改为 SD 独立 SPI3（17/16/18/47），验证稳定 |
| 升压描述（独立 LDO / VGH 电容耐压红线） | 实测架构为 **EVK011 板上分立 boost 由屏 COG 从 FPC pin2(GDR) 自主驱动**，MCU 不输出 GDR/RESE，仅供 J2-16 VCI 3.3V；PRD 4.3 高压电容红线对现硬件不适用（高压电容组 EVK011 已集成） |
| 屏型号 BNU253F29 | 实际点亮 **BBU253F33HP-M7**；且 PRD 5.1 驱动选型 EPDiy V7 与现实（GxEPD2 定制）不符 |
| INMP441 DOUT=41 / 马达=6 | 与实测接线（I2S_DOUT=6、马达预留=41）互为镜像冲突 |

### 6.2 实测接线（权威基线，详见 [WIRING_DIAGRAM.md](WIRING_DIAGRAM.md)）

EPD 七线 7/8/9/10/11(BS)/12/13（经 EVK011）；I2S 4/5/6；五向导航 1/2/14/15/21 +
SET=42/RST=40；SD 独立总线 16/17/18/47；UART0 43/44；状态灯 GPIO48 板载 WS2812。

### 6.3 V2.0 新增硬件的引脚预算（核心约束）

N16R8 模组占用：Octal PSRAM 33-37、嵌入式 Flash 26-32、strapping 0/3/45/46、USB 19/20。
扣除上述与 §6.2 已占用后，**当前完全空闲引脚仅剩 GPIO38、39（I2C 电量计/RTC 预留）与 GPIO41（马达预留）**。

V2.0 新增硬件需求：INMP441 数据线 ×1（时钟可与输出共享，见 §8.4）+ 马达 ×1 + 蜂鸣器 ×1 +（可选）I2C 电量计 ×2。

| 候选来源 | 代价 | 建议 |
|:---|:---|:---|
| **取消蜂鸣器**（提示音改由 MAX98357 扬声器播放短音频样本） | 零 | ✅ 强烈建议：省 1 元件 1 引脚，且 PRD BOM 本就未列蜂鸣器 |
| GPIO41 → 马达 | 已预留 | ✅ 直接使用（外接 MOS 管驱动） |
| **BS 省线释放 GPIO11**（J2-10 板侧短接 GND，`EPD_BS_PIN` 改 -1 重烧） | 接线变更 + 拆线时序风险（WIRING_DIAGRAM §2.1 铁律） | 🔁 INMP441 DOUT 的首选来源 |
| GPIO38/39 挪用（放弃/迁移 I2C 预留） | 失去电量计 I2C 位 | 备选（电量降级为 BS 脚分时采样 hack） |
| GPIO19/20（放弃 USB） | 失去 USB 烧录/调试 | ❌ 不建议 |

> 注：WIRING_DIAGRAM.md §三总表仍残留旧 6 键命名（KEY_A~F），与 §2.3 五向开关节不同步，
> 随本次 PRD 修订一并更正。

---

## 七、性能指标现状核对（PRD 8.1）

| 指标 | PRD 目标 | 现状 |
|:---|:---:|:---|
| 全屏刷新 | <2.5s | ✅ 1500ms 标称 + 数据加载，余量充足 |
| 局部刷新 | <800ms | ✅ 350ms 标称 |
| 按键响应 | <50ms | ✅ 扫描 20ms + 去抖 50ms（按下确认即触发语义，震动反馈待马达接入后兑现） |
| 开机启动 | <3s | ◐ 未专项测量；启动链（NVS→SD→屏→音频→按键→Wi-Fi→词库）顺序已定型 |
| 待机功耗 <5µA | <5µA | ❌ SoC 深睡未实现；且与待机页"每 5 分钟全刷引文"互斥，需定义"深睡+定时唤醒刷新"架构模式（PRD 指标需补前提条件） |
| 续航 ≥15 天 | ≥15 天 | ❌ 未验证；电池仅 1000mAh，电流预算 ~310mA 峰值（WIRING_DIAGRAM §2.5） |

---

## 八、V2.0 新增功能实现路径

### 8.1 错词本 + 收藏（成本最低、收益最高，建议首个动工）
- 后端：LearningRecord 加 `ConsecutiveWrong`/`IsCollected` 两字段 + 迁移；
- 固件：SrsNode/本地记录同步扩展；复习队列按 ConsecutiveWrong>0 过滤优先推送；
- 管理端：错词排行页（数据源就绪后 ECharts 直接接入）。
- 前置决策：左右键自评语义与现行 SET/RST 语义的统一（§三交互总表重排）。

### 8.2 震动反馈
- 新增 `haptic.c`：GPIO41 + MOS 管，统一事件表（20/30/50/100ms/双短震，对齐 PRD 5.4）；
- 在 button_handler 回调与模式切换点埋点。
- 前置条件：无（引脚已预留）；与蜂鸣器取消决策绑定后音效走扬声器。

### 8.3 阅读模式（工作量最大，被字库卡脖子）
- **阻塞点**：现 [cjk_font.c](../InkWord_Firmware/src/cjk_font.c) 为《传习录》24px 子集楷体，无法渲染任意 TXT。
  需扩展 [tools/gen_cjk_font.swift](../InkWord_Firmware/tools/gen_cjk_font.swift) 生成 GB2312 常用 3500 字集
  （24px 单级约 250KB Flash；16MB Flash 充裕，DRAM 不受影响）。
- 字号档位建议沿用 epd_gfx 现有 9/14/18/24pt 四级（`epd_gfx_draw_text` 已支持），
  而非 PRD 的 16/20/24/32pt 另造一套。
- 分页引擎：SD 流式读取（勿整书入 DRAM）+ NVS 每 5 秒存页码；
- 入口：融入现有"长按下=循环切模式"，成为第四模式；
- 刷新：逐页全刷（对齐 PRD），局刷计数策略沿用 refresh_scheduler。

### 8.4 语音跟读（最后动工）
- I2S 全双工：ESP32-S3 单控制器 STD 模式支持 TX/RX 共享 BCLK/WS ——
  即 BCLK=4、WS=5 不变，MAX98357 DIN=6 保持，INMP441 仅新增 DOUT 一根线（引脚见 §6.3）；
  录音期间向 TX 写静音零样本避免功放杂音。
- **PRD 需先补需求**：评测算法（本地能量包络/DTW vs 云端评分）与通过阈值验收标准。
- PSRAM 8MB 充足（录音缓冲走 PSRAM）。

### 8.5 词库容量扩展（PRD 隐性需求）
- `MAX_WORDS=64` → 词库结构体数组迁移 PSRAM（参照既有 DRAM→PSRAM 优化经验），
  或 SD 索引分页加载；词库 JSON 字段对齐 PRD 7.2（Root/Inflections/Source/Grade）。

---

## 九、PRD 修订清单（动工前完成）

1. **4.2 GPIO 表整体替换**为 gpio_config.h 实测版（§6.2）；删除 GPIO3/45/46 相关分配；
2. 4.1 BOM：去掉蜂鸣器（音效改扬声器）；补 SET/RST 侧键条目；屏型号改 BBU253F33HP-M7；
3. 4.3 红线表：升压条目改写为"EVK011 COG 自主升压，MCU 仅供 VCI"；高压电容红线标注不适用于现硬件；
4. 5.1 驱动层：EPDiy V7 / ESP-IDF → GxEPD2 定制 / Arduino+PlatformIO
   （或单列"IDF 迁移"决策项，动机含 BLE coex 解锁）；
5. 5.3 交互表按 §三 重写：补 SET/RST、解决长按中键冲突、定义左右键自评；
6. 5.6 刷新伪代码对齐 refresh_scheduler 实际（阈值计数 + 分页面阈值）；
7. 6.2 数据表：决策学习者账户体系（V1 设备即用户 vs 引入 UserId）；
   Words 补 Root/Inflections/Source/Grade；LearningRecords 补 ConsecutiveWrong/IsCollected；
8. 新增"移动端（InkWord_App）"章节或范围声明；
9. 8.1 待机功耗/续航指标补充架构前提（深睡+定时唤醒模式定义后）；
10. 8.4 语音跟读补评测算法与验收标准。

---

## 十、建议实施顺序

```
P0  修订 PRD（§九清单）＋ WIRING_DIAGRAM §三总表更新      ← 文档对齐，半天
P1  错词本 + 收藏（后端字段 → 固件过滤 → 管理端排行）      ← 最低成本最高收益
P2  震动反馈（haptic.c + 事件表埋点；蜂鸣器取消决策落地）   ← 引脚已预留
P3  阅读模式（先投资 gen_cjk_font 3500 字链 → 分页引擎 → 第四模式）
P4  I2S 全双工改造 + 语音跟读（PRD 补需求后）
P5  SoC 深睡 + 定时唤醒刷新架构（待机页/续航指标兑现）
P6  BLE coex / IDF 迁移评估（如需解锁 BLE 配网主链路）
```
