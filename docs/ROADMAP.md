# InkWord 后续版本迭代路线图（v1.1 → v2.0）

> **版本** | 路线图 v1.1（2026-08-24 全科化重排；v1.0 同日定稿，备份 .bak1）
> **依据** | 代码库实测状态（固件 8 env / 后端 AI 管线 / App 骨架）× [PRD_V2.1.md](PRD_V2.1.md) §九 × [PRD_GAP_ANALYSIS.md](PRD_GAP_ANALYSIS.md) × 四份设计文档（MENU_DESIGN v1.3 / QUIZ_DESIGN v0.1 / AI_CHAT_MODE / AI_SPEECH_ASSESSMENT）× 全科学习机方向分析（G0-G6，2026-08-24）
> **维护原则** | 随版本交付滚动更新，状态冲突时以代码与真机实测为准；每版交付后在此追加「实际交付 vs 计划」差异注记。
> **排序逻辑** | 验证债 > 新功能；设计已冻结的先做；管线已就绪的次之；架构迁移最后；**泛化设计前置、泛化实现按价值排期**（早期版本只注入设计约束防返工，不提前实现）。
> **v1.1 重排摘要** | 定位升级为「全科记忆背诵机」（G0）：v1.1 不动；v1.2/v1.3 注入泛化约束（QUIZ 抽象 / 词书 Deck 化）；新增 v1.4 全科地基版、v1.5 全科体验版；QUIZ P2 自 v2.0 前移 v1.5；账户决策自 v2.0 前移 v1.4。

---

## 〇、现状基线（2026-08-24）

| 层 | 已完成 |
|:---|:---|
| **固件** | P1-P5 全落地：五学习模式（闪卡 / 听写 / 复习两态化 / 阅读 / 错词本+收藏）+ FSRS-4.5 + 深睡架构（power_manager）+ OTA 双分区 + 多屏兼容 8 env + 快捷菜单 v1.3 分组 + AI 对话（MODE_CHAT）+ 跟读评测（听-跟一体流）+ 音频同步 + 今日统计（2026-08-24 O1-O5：遮蔽态重设计 / 复习到期词表 / 菜单分组 / 学习概况页） |
| **音频硬件** | **2026-08-24 定档 ES8311+NS4150B CODEC**（取代 MAX98357A+INMP441）：I2C 控制 38/39、I2S 播放 4/5/6 + 录音 DOUT=11（BS 省线后）；[es8311.c](../InkWord_Firmware/src/es8311.c) 驱动（移植 esp-adf）与 [mic_recorder.c](../InkWord_Firmware/src/mic_recorder.c)（ES8311 ADC 全双工）均已就绪，待上板验证 |
| **云端** | .NET 8 五层 + 管理端四模块 + AI 内容管线（AiContentService/Job/审校端点）+ FsrsService（与固件对拍）+ 语音评测端点 + Word 五字段（Example/Root/Inflections/Source/Grade） |
| **App** | Flutter 骨架：BLE 双侧架构、帧协议、排版测试（配网主链路实际走 AP/LAN 直传） |
| **设计文档** | MENU_DESIGN v1.3、QUIZ_DESIGN v0.1（**未实施**）、AI_CHAT_MODE、AI_SPEECH_ASSESSMENT、PANEL_COMPAT_DESIGN |

**核心判断**：功能建设已超前于 PRD 里程碑叙述；当前主要债务是**验证债**（多笔「固件已写、待上机」）与**内容/生态债**（多词书、App、管理端分析），而非新功能。

**全科化定位（G0，随本版定稿）**：设备是**全科记忆背诵机**——一切「看题面 → 回忆 → 揭晓 → 自评」的知识（单词 / 古诗文默写 / 公式定理 / 化学方程式 / 政史地知识点）皆可承载；**不做**计算题 / 作文 / 实验演示类「做题机」。现有资产约 70% 科目无关（FSRS 引擎、learning_state 连错收藏、刷新调度、增量同步、音频管线、快捷菜单框架），被英语绑定的核心是**数据模型**（[Word.cs](../InkWord_Backend/src/InkWord.Core/Entities/Word.cs) 英语特化字段）与**卡片渲染**（单词卡版式）——泛化改造集中于这两处。

---

## 一、全科化方向 × 优先级整合（本次重排核心）

| 方向 | 内容 | 与原路线关系 | 处置与落点 |
|:--|:--|:--|:--|
| **G0** 定位与规范 | 记忆背诵机边界 + Subject/Deck/Item Schema 规范 | 新增 | **文档先行**：定位随本路线图定稿；PRD 增补清单见 §九，随 v1.2 设计期完成 |
| **G1** 数据模型泛化 | Word → Subject/Deck/Item 三表（公共字段 + PayloadJson 混合模型） | 与 v1.3 词书切换强耦合 | v1.3 只做**结构预留**（manifest 留字段、按卡组语义命名）；**实施在 v1.4** |
| **G2** 渲染抽象 + 第二科目 | 版式按科目分派；语文古诗文默写 | 新增；字库投资与阅读模式三级字库同源 | **v1.4 主体** |
| **G3** 题型引擎 | 四选一 → 填空 / 判断 / 配对 | QUIZ P1 已冻结于 v1.2；P2 原排 v2.0 | P1 实施按泛化抽象写（零额外工期）；P2 + 新题型**前移 v1.5** |
| **G4** 自定义科目闭环 | App/网页卡组编辑器 + AI 批量生成 + 字库子集下发 | 依赖 v1.3 AI 管线与 App 一期 | 字库子集随 **v1.4**（古诗生僻字刚需）；编辑器与批量生成在 **v1.5** |
| **G5** 学习策略 | 科目级配额 / 考试倒计时 | 依赖 v1.2 每日计划 | **v1.5** |
| **G6** 生态与账户 | 卡组商店 / UGC / 账户体系 | 账户决策原排 v2.0 | 决策**前移 v1.4**（自定义卡组归属需要）；实施分层：v1.5 轻账户 / v2.0 完整 |

**冲突消解**（重排动因，共 5 条）：

1. v1.3「词书切换」若按英语词书硬编码，v1.4 泛化即返工 → v1.3 池 manifest 预留 `deckId/subject/payloadType` 字段，模块按卡组语义命名；
2. v1.2 QUIZ 若直接读英语字段（释义当选项）→ 全科题型返工 → quiz_session 以「词条索引 + 通用文本接口」出题，不绑 Word 字段名；
3. 账户决策被自定义卡组（G4）提前 → 决策点移 v1.4，实施分层（见 G6）；
4. 词池 4000 / LR sparse 上限对单科目够用，多卡组超限 → v1.4 卡组管理器（活跃单载 + SD 索引 + learning_state 按卡组隔离）；
5. v1.1 验证债与全科化正交（ES8311 / 提示音 / 字库 / 功耗为全科共用资产）→ **v1.1 顺序与内容不变**。

---

## 二、v1.1 —— 真机闭环版（收尾验证，约 1-2 周）【本次不动】

> 主题：清零「固件已写、待上机」的全部验证债，达成可日常使用状态。
> 注：音频链路（ES8311）与提示音为全科科目（古诗韵读 / 课文朗读）共用资产，本版优先级不变。

| # | 事项 | 依据 |
|:--|:--|:--|
| 1 | **ES8311 音频链路 bring-up**：升压/供电检查（5V）→ I2C 探测（0x18）→ 播放（audio_player）→ 录音（mic_recorder，32bit 槽 >>16 截取验证）→ 跟读评测全链路回归 | 2026-08-24 硬件定档，驱动已写未上板（README §1.3） |
| 2 | **O1-O5 真机回归**：遮蔽态视觉（大问号 / 拼写线节奏）、复习词表滚动与自评出队、菜单分组导航、统计跨日结算 | 2026-08-24 落地，未上机 |
| 3 | **阅读模式上机验证**：三级字库渲染、翻页残影、进度恢复、字号切换重定位 | PRD §5.2 标 ◐ 待上机 |
| 4 | **深睡功耗实测**：<5µA 待机与 ≥15 天续航指标，静默心跳会话全链路 | P5 架构落地待上机（PRD §8.1 两项 ❌） |
| 5 | **nvs 分区扩容**：LR_SPARSE_MAX 300 活跃词上限根治（16MB Flash 充裕；修订分区表需评估 OTA 双分区兼容） | learning_state.c 容量保护注释 |
| 6 | **提示音 WAV 样本**：按键「滴」/ 自评「滴答」/ 模式「滴--」/ 错误「嘟-」经扬声器（`/sdcard/audio/ui/`） | PRD §5.4 音效列全部「待功放」（功放已随 CODEC 就绪） |
| 7 | **TINY 档版面微调**：INFO 值列超宽、拼写线 ~8 位截位、按键说明单列化 | 代码内 3 处「bring-up 再调」注记 |
| 8 | **开机启动 <3s 专项测量** | PRD §8.1 ◐ 未测量 |

**验收**：真机全路径回归清单全绿（MENU_DESIGN §10 范式）+ 功耗实测报告 + 经 OTA 灰度推送一版。

---

## 三、v1.2 —— 学习体验版（约 2-3 周）【注入泛化约束 ×2】

> 主题：客观题作答 + 学习计划性——今日统计（O5）的「另一半」。

| # | 事项 | 依据 |
|:--|:--|:--|
| 1 | **QUIZ 四选一 P1**：P1-a quiz_session 出题核心 + native 测试 → P1-b MODE_QUIZ + ui_draw_quiz + 反馈/小结页 → P1-c 菜单 [学习] 组「快速测验」项 + 按键说明组 + 文档。**泛化约束：出题核心以「词条索引 + 通用文本接口」取题干与选项，不直接绑英语字段**——v1.4 全科目复用零返工 | [QUIZ_DESIGN.md](QUIZ_DESIGN.md) v0.1 切分就绪，设计冻结；本路线 §一 G3 |
| 2 | **每日学习计划**：每日新词量 N（到期词优先 + 新词补足的「今日任务」语义；复习空态页升级为「今日任务完成 ✓」闭环）；**配额与任务结构按卡组维度预留字段**（v1.5 科目级配额前置） | O5 已有统计无定量；百词斩「今日进度/定量学习」范式 |
| 3 | **设置页**（菜单 [系统] 组二期位）：每日新词量 / 发音开关 / 震动开关 / 字号 | MENU_DESIGN 二期预留 |
| 4 | **电量 ADC**：⚠️ 原方案失效——GPIO38/39 已定档 ES8311 I2C（2026-08-24），需重新选址（分压 ADC / I2C 总线复用），gpio_config.h「38/39 I2C 电量计预留」注释同步修订；INFO 页电量行占位已留 | main.cpp bat=100 TODO；PRD §5.7「电量行待 ADC」 |
| 5 | **例句上屏确认**：WordEntry.example 字段与 AI 内容管线均已就绪，词卡版式若未消费则补排版（词根行下方 / 详情翻页） | word_parser.h；AI 管线 Nightly 批量生成 |

**验收**：QUIZ native-test 全绿（出题接口无英语字段硬编码）+ 真机 10 题轮次体验通过；「今日任务」从配额到统计全闭环。

---

## 四、v1.3 —— 内容生态版（约 3-4 周）【注入泛化约束 ×2】

> 主题：从「单词库设备」到「内容平台」——多词书 + 管理端数据闭环 + App。

| # | 事项 | 依据 |
|:--|:--|:--|
| 1 | **词书/卡组切换（Deck 化设计）**：SD 多词库 + 云端下发 + word_parser 多词池 + 菜单入口；**池 manifest 预留 `deckId/subject/payloadType` 字段、模块按卡组语义命名**（v1.4 泛化零返工；LR sparse 按词库 count 隔离已有先例） | PRD §7.1 规划；菜单二期预留；本路线 §一 G1 |
| 2 | **管理端学习分析模块**：错词排行 / SRS 分布（srs-distribution 已有 FSRS 口径）/ 今日统计云端聚合（设备上报数据即可聚合，无需新协议） | PRD_GAP §四「学习分析无独立模块」 |
| 3 | **教材词书内容填充**：Grade/Source 溯源词书（人教版分年级）批量导入 + AI 管线批量生成例句/助记 + 人工审校；**AI prompt 模板参数化（科目占位符）**，为 v1.5 全科批量生成预留 | Word 五字段 + ai-generate / ai-pending 端点已就绪 |
| 4 | **InkWord_App 一期**：AP/LAN 配网主链路 + 词库管理 + 学习数据可视化（今日统计 / 连续天数上大屏）；**导航预留二期「卡组编辑器」入口位** | PRD §十一；App 骨架已投入 |

**验收**：真机切词书不丢阅读进度/收藏（LR count 隔离验证）；App 完成一次配网 + 换词书全流程。

---

## 五、v1.4 —— 全科地基版（新增，约 3-4 周）

> 主题：Subject/Deck/Item 模型落地 + 渲染分派 + 第二科目（语文古诗文）跑通——「全科」从设计变为可学。

| # | 事项 | 依据 |
|:--|:--|:--|
| 1 | **后端模型泛化**：Subject / Deck / Item 三表迁移（公共字段 Front/Back/Audio + PayloadJson 承载科目特有内容；英语整体迁为 subject 之一）；`/admin/words/export` 协议 v2——cloudId / Version / ChangeType 语义不变，新增 subject / 版式类型字段（旧固件忽略新字段，向下兼容） | G1；增量同步与 FSRS 双端一致红线不破坏 |
| 2 | **固件卡组管理器**：SD 多卡组索引 + 活跃卡组切换（复用 v1.3 词书切换骨架，manifest 预留字段兑现）+ learning_state 按卡组隔离（LR sparse 键加 deck 维度）；词池 4000 上限按「单活跃卡组」重估 | §一冲突 4；DRAM→PSRAM 迁移经验 |
| 3 | **渲染器分派**：卡片版式按 subject 版式类型注册——单词卡（现版式）/ 问答卡（多行 CJK）/ 古诗卡（拼音标注行，复用 IPA 点阵分派经验） | G2；epd_gfx 多字号档位已有 |
| 4 | **第二科目「语文古诗文」**：部编版必背篇目 Deck + 默写模式（上下句遮蔽/揭晓，复用 SET 遮蔽与听写同构逻辑）+ 韵读音频走 audio_sync | 默写与闪卡遮蔽同构、教材刚需；首推科目论证见全科化分析（内容形态同构度最高） |
| 5 | **字库子集下发**：后端按 Deck 字符集对已装三级字库做覆盖率校验，生僻字点阵子集随卡组下发（SD 存放） | 古诗生僻字超纲；按需子集优于固化更大字集；与阅读模式字库同源 |
| 6 | **账户体系决策**（决策不实施）：自定义卡组归属 → 定「v1.5 App 轻账户只管内容 / v2.0 完整 UserId」分层路线 | G6 前移；PRD_GAP §四语义级差异 |

**验收**：真机英语 ↔ 语文切换后学习 / 复习 / QUIZ 全路径绿；古诗文「遮蔽 → 揭晓 → 自评 → FSRS」闭环；导出协议 v2 对旧固件兼容验证；PSRAM / Flash / NVS 预算实测报告。

---

## 六、v1.5 —— 全科体验版（新增，约 3-4 周）

> 主题：题型扩展 + 自定义科目闭环 + 学习策略——从「内置全科」到「用户可造科目」。

| # | 事项 | 依据 |
|:--|:--|:--|
| 1 | **题型扩展**：QUIZ P2（T3 听音辨义 / 2×2 方向直选快答 / 同首字母干扰项，自 v2.0 前移）+ 判断题（全科最低成本新题型）+ 配对题（四选一变体，化学式 ↔ 名称） | QUIZ_DESIGN §8；题型三件套（渲染 / 作答 / 评分映射）抽象已随 v1.2/v1.4 就绪 |
| 2 | **App 二期 · 卡组编辑器**：自定义科目（版式模板选择）→ 条目录入 / Excel 导入 → 推送设备（增量同步 + LAN 直传复用）；轻账户（内容归属，学习记录仍挂 DeviceId） | G4；账户分层决策（v1.4 #6） |
| 3 | **AI 批量生成卡组**：按科目 prompt 模板（课文全文 → 默写卡 / 注音 / 译文；知识点 → 问答卡）→ 人工审校 → Version++ 下发 | 复用 AiSuggestion 审核管线，设备零改动；AI 后端化红线 |
| 4 | **学习策略**：科目级每日配额（英语 N 词 + 古诗 M 首）+ 跨科目今日统计 / 菜单角标 + 考试倒计时模式（按日期反推优先推到期卡） | G5；依赖 v1.2 每日计划 + v1.4 多科目 |

**验收**：自建科目 ≥10 条卡推送设备并完成学习闭环；双科目配额与统计口径正确；AI 生成卡组「审核 → 下发 → 学习」全链路演练。

---

## 七、v2.0 —— 平台化版（季度级）【瘦身：QUIZ P2 / 账户决策已前移】

> 主题：架构升级解锁被 Arduino 框架封锁的能力 + 账户实施 + 内容生态平台化。

| # | 事项 | 依据 |
|:--|:--|:--|
| 1 | **ESP-IDF 原生迁移评估**：动机 = BLE coex（解锁 BLE 配网主链路，现为默认禁用防崩溃循环）；EPDiy V7 届时重评 | PRD §5.1 远期评估 / P6 |
| 2 | **账户体系实施**：UserId 挂接 + 多设备同步（决策已于 v1.4 #6 定，多设备同步 LWS 策略已有） | PRD_GAP §四 |
| 3 | **卡组生态**：教材包商店 / UGC 分享 / 付费卡组（依赖账户与 v1.5 G4 闭环） | G6 |
| 4 | **摇杆接入评估**（硬件候选）：⚠️ 引脚预算随 ES8311 定档进一步恶化（38/39 I2C + 11 DOUT 已占用），评估 ADC 候选通道与五向键事件融合 | 交互设计规范「摇杆事件融合」预留 |
| 5 | **结构/量产准备**：外壳 FPC 压板（弯折半径 ≥5mm）、三色屏 UX 降级复审 | PRD §十风险表 🔴 项 |

---

## 八、横向支线（随硬件事件插入，不占版本号）

- **三块 bring-up 屏**（e042bw / wft0290 / opm021eb env 已建、标注「bring-up 前勿 upload」）：硬件到货即插入当期版本；**4.2 寸屏是长内容科目（古文全文 / 材料阅读）的大屏出口**，bring-up 后评估全科版式；
- **OTA 灰度通道**：每版必经（双分区回滚 + MD5 已就绪），v1.1 起建议每次真机验证后走完整灰度流程演练；
- **硬件变更窗口**：音频链路若 bring-up 发现 ES8311 模块缺陷（克隆板风险，MAX98357A 有前车之鉴），回退评估单独启动。

---

## 九、PRD 增补清单（G0 文档交付，随 v1.2 设计期完成）

1. 新增「全科定位」章节：记忆背诵机边界（做什么 / 明确不做什么）；
2. Subject / Deck / Item Schema 规范定稿（公共字段 + PayloadJson 混合模型）；
3. 科目优先级排序：语文古诗文 → 理科公式 / 化学方程式 → 政史地知识点（按与问答卡同构度）；
4. 默写题型定义（上下句遮蔽语义、评分映射 quality）；
5. 字库子集下发协议（Deck 字符集 → 点阵子集，SD 存放与覆盖率校验）；
6. 账户分层策略（v1.5 轻账户 / v2.0 完整账户）。

---

## 十、版本交付记录

| 版本 | 状态 | 计划 vs 实际差异 |
|:---:|:---:|:---|
| v1.0（本文档基线） | ✅ 2026-08-24 | — |
| v1.1 真机闭环版 | 🟡 软件项完成 2026-08-24，真机项待硬件 | T1.5 NVS 扩容（0x30000 + LR_SPARSE_MAX 4000）/ T1.6 提示音（ui_sfx + 4 样本 + 埋点）/ T1.8 开机埋点已交付且构建绿（RAM 18.7%/Flash 77.4%）；T1.1 ES8311 bring-up / T1.2-T1.4 真机回归与功耗实测 / T1.7 TINY 微调待硬件窗口（不阻塞后续版本软件开发） |
| v1.2 学习体验版 | 🟡 软件侧全量完成 2026-08-24，真机验收待硬件 | T2.1-T2.7 全部交付：QUIZ P1（quiz_session 纯 C 核心 + native 7 用例绿、MODE_QUIZ 临时视图第四先例、菜单入口）/ daily_plan 每日计划（复习空态升级 + 概况页 n/goal）/ settings_ui 设置页（set_* 四键 + 发音/震动/大字门控）/ i2c_bus 共享总线抽象 + max17048 电量计（代码就绪待模块）/ 例句上屏（ui_body_stream 消费 WordEntry.example）；验收差异：真机 10 题轮次/设置项重启保持/电量行实测待硬件，quiz_session 无英语字段硬编码 grep 复核通过 |
| v1.3 内容生态版 | 🟡 软件侧全量完成 2026-08-24，真机验收待硬件 | T3.1 词书 Deck 化交付：deck_manager（manifest 扫描/活跃切换，subject/payloadType 预留字段解析即忽略）+ 菜单 [学习] 组「词书选择」二级页（镜像模式页范式 + 滑动窗口）+ deck_flow_switch 编排（三级递降重载 + learning_state_reload 显式作废 + rd_* 卡组后缀隔离，切书不丢阅读进度）+ 附带菜单 20px 剪影图标前缀（gen_menu_icons.py 12 枚，v1.2 对比分析采纳折中方案）；T3.2 学习分析：真正增量 = 今日统计云端聚合（today-stats 端点 + TodayStatsResp，LastStudiedAt/ReviewCount/LastQuality 近似口径；错词排行 wrongbook 模块与 SRS 分布 dashboard 饼图此前已交付，未重复建 learning-analysis 模块）+ 看板「今日学习分析」条（首学/复习/正确率/平均质量）；T3.3 AI prompt 参数化：subject 科目占位符全链透传（AiGenerateReq→Job→BuildPrompt，缓存键带科目维度保默认键兼容，夜间任务签名同步）+ 管理端生成弹窗科目下拉（v1.5 全科生成入口）；T3.4 App 一期：固件 LAN 四端点（GET decks/stats + POST upload 流式落 SD+manifest upsert / active 复用切书编排，max_uri_handlers 4→6，storage 补写能力）+ Flutter 三主页面导航壳（设备/词书/统计，IndexedStack 保活）、词书列表切换/上传（file_picker）与今日统计/连续天数可视化、「卡组编辑器」二期入口位；构建态：固件 RAM 19.8%/Flash 77.9% + native 7 绿、后端 sln 0 错、Angular/Flutter analyze 全绿；真机验收（切书进度/收藏口径/App 换书全流程）待硬件 |
| v1.4 全科地基版 | 🟡 软件侧全量完成 2026-08-24（T4.1-T4.6），真机验收待硬件 | T4.1 后端模型泛化：Word→Subject/Deck/Item 三表（Word 侧纯 Id 关联不建 FK/导航，Deck→Subject 单向导航 + Restrict 删除；Word +SubjectId/DeckId/Front/Back/PayloadJson 五列）+ 幂等迁移双路径（新库 EnsureCreated 建表，存量库 t41Sql 补齐：CREATE IF NOT EXISTS 建两表三索引 + 固定 Guid 种子 en/junior ON CONFLICT DO NOTHING + 仅补空回填，防空覆盖）+ 协议 v2 双写（/admin/words/export 加 v=2 标记与 subject/deckId/payloadType/front/back/payloadJson 六字段，DeviceController SyncWords 同源映射；Word.SubjectId/DeckId→Decks/Subjects 字典查 Code，null/失配兜底 en/junior/word-card；cloudId/Version/ChangeType 语义不变——FSRS 双端一致红线）+ Create/Update/ImportCsv/Seed Front/Back 镜像随写 + native 固化用例 test_word_parser_ignores_protocol_v2_fields（六新键 + futureNested/futureList/futureNull 未来扩展形态，cJSON 按名取值未知键天然忽略，不用环境变量永远执行）；构建态：后端 sln 0 错 + 39 测试全过、native 8 绿 + 2 skipped（WORDS_JSON 可选用例）；排障存档：用例 JSON 缺根对象闭合 `}`（多轮文本比对无效后独立最小复现 + hex dump 定位 ep=字符串末尾，佐证「心算验证 vs 编译器实际字节」陷阱；另实测 test/cJSON vendored 1.7.19 的 ParseWithLength 传 require_null_terminated=0，与上游默认不同，垃圾尾容忍）。T4.2 固件卡组管理器升级：LR04 按组隔离（magic LR03→LR04，blob 头加 deck 短串 8B 三重校验 magic/deck/count；NVS 键按组分派——默认组沿用 lr_state、其余 lr_st_<id> ≤15 键上限，头串与键双保险防 manifest 改 id 错位；reload 语义反转：切书=旧组先落盘+新组从其键恢复，替代 v1.3「切书作废」先例，学习进度与 rd_* 阅读进度同口径互不丢；lr_stats 保持全局跨组累计）+ manifest 预留字段兑现（deck_info_t +subject[8]/payload_type[16]，scan 解析缺省回退 en/word-card，upsert 五参可写，新增 active_subject/active_payload_type getter 供 T4.3 分派）+ 词池上限重估注释（4000=单活跃卡组口径，多组只多占 SD 不多占 PSRAM）；init/reload 签名加 deck_id（main 两调用点同步）；构建态：inkword-s3 绿（RAM 19.9%/Flash 78.0%），native 不涉及（src filter 未变）；真机验收（切书往返进度保留/LR04 旧态作废重学）待硬件。T4.3 渲染器分派：card_layout 纯 C 分派（strcmp 三版式 + NULL/空/未知/大小写敏感均回退 word-card，英语开箱行为不变红线）+ native 2 用例挂 srs runner（setUp/tearDown 单份纪律防 duplicate symbol）；main.cpp 分派接入（ui_draw_content 开关分流，word 现版式零改动）：qa 版式 = 题干多行 CJK 量测 clamp 半屏行数常驻 + 答案 meaning 流揭晓分页 + 遮蔽提示「[SET] 揭晓答案」，poem 版式 = 诗行（正文字号大一级，绝句两句内）+ 拼音行 16px（音标渲染链复用）+ 译文分页 + 遮蔽提示同构（SMALL 档居中问号会压拼音行，改左对齐小字）；ui_mean_geom 参数化（+w/+layout：qa 答案区让题干行、poem 译文区让诗头，行数按剩余高度重建至少 1 行，页码指示基线跟随 top），翻页链 ui_mean_page_step 经 total_pages 自动同口径；payloadJson 结构化载荷（默写上下句）留 T4.4 消费；构建态：native 10 绿 + 2 skipped、inkword-s3 绿（RAM 19.9%/Flash 78.0%）。T4.4 语文古诗文 Deck + 默写：后端 PoemSeeder（SeedData/poems_cb.csv 部编版必背名联 15 条 12 首，一上~初中七下；zh/poems 固定 Guid 种子，Code=poems 5 字符 ≤ NVS 后缀预算 7；幂等判据 = Decks 含 poems，英语库存在也可补第二科目；Version 接 max 续增随增量下发）+ 字段映射契约（text=下句默写答案 / root=上句题面 / phonetic=下句拼音 / meaning=译文，PayloadJson={"prev":上句} 结构化留云端/App 二期，设备零解析）；固件默写态 ui_draw_poem_dictation（poem 版式 MODE_DICTATION 分支：上句题面常驻首诗行位 + 下句遮蔽逐全角字符空框=字数线索 + 拼音行同遮防泄底 + 揭晓后上下句均在屏；收藏星标前移遮蔽态照画）；native 固化用例 test_word_parser_poem_dictation_fields（设备端契约：四要素入 WordEntry + 缺键空串兜底 + payloadJson 不污染）；AI 注音译文零代码（T3.3 subject=zh prompt 已参数化，种子为开箱基线）；构建态：native 11 绿 + 2 skipped、后端 sln 0 错 + 39 测试、inkword-s3 绿；真机验收（英语↔古诗切换/默写「遮蔽→揭晓→自评→FSRS」闭环）待硬件。T4.5 字库子集下发：工具链 + 固件级联全通——① swift 工具 --subset 模式（gen_cjk_font.swift：读后端 charset 文本 − 主集 bin 已收码点 = 差集，空差集 exit(0)；差集字符同管线渲染三级位图 → ./deck_<id>.bin CKF1 同格式，不重生 c/h 与引文表；实测鿕鿑貔貅差集 4 字 688B 头/几何/体积均与规格吻合）；② 后端 GET /api/admin/decks/{code}/charset（AdminDeckController 新建：Deck 按 Code 定位 + 纯 Id 关联显式过滤，设备端可渲染九列 text/phonetic/meaning/example/root/inflections/source/grade/tag 去重字符流 text/plain，payloadJson 零解析不收集）；③ 固件 cjk_font_sd 模块（新建：stdio 直连 VFS 二进制读——storage_read_text 是文本语义不适用；PSRAM 整体装载 + magic/levels/几何/尺寸四重校验拒异源 bin 防花屏，cell={16,20,24}/stride={2,3,3} 与主集/生成工具三方同源硬校验；lookup 二分镜像主集逻辑）+ cjk_text adv_one 级联接线（主集 miss → 子集，几何同构 ink_span/blit 参数直接沿用；reader_engine 同源副本未动待上机合并时统一接入，standby 引文主集全收不涉及）；④ 编排：load_active_words 开头随活跃卡组同步装载/卸载子集（setup 与 deck_flow_switch 共用链路单点覆盖，无文件=主集已覆盖的正常路径静默通过；LAN 推送不自动切组不受影响）；子集拷入 SD /fonts/ 手动流（v1.5 随卡组下发自动化）；native 4 用例（手工 fixture 三级 lookup 偏移特征字节/坏 magic/几何篡改/截断拒载/不存在=1 清空语义 + swift 产物跨实现直通 DECK_BIN 门控锁死 Swift 生成端↔C 消费端同构契约）；构建态：native 17 用例 15 绿 + 2 skipped（DECK_BIN/WORDS_JSON 可选）、后端 sln 0 错 + 39 测试、inkword-s3 绿（Flash 78.1%）；真机验收（古诗 Deck 生僻字渲染/切组子集热换）待硬件。T4.6 账户体系决策：[ACCOUNT_MODEL_DECISION.md](ACCOUNT_MODEL_DECISION.md)（ADR-001）定稿——决策不实施：**v1.5 App 轻账户只管内容归属（+Account 表 + Deck.OwnerId，固件零改动红线）/ v2.0 完整 UserId + 多设备（Device.UserId 既有可空列兑现 + LWS 聚合不迁移）**，LearningRecord 挂 DeviceId 两版均不动；三方案对比否决 A（维持设备即用户：UGC 无归属不可接受）与 C（v1.5 一步到位：墨水屏无键盘绑定交互不现实 + FSRS 双端一致红线）；勾稽 PRD_GAP §九 6 决策项闭环，v1.5 T5.3 / v2.0 #2 实施边界均已写入决策文档 |
| v1.5 全科体验版 | 🟡 软件侧全量完成 2026-08-25（T5.1~T5.5） | T5.1 QUIZ P2：① quiz_session 核心题型轮换（quiz_question_t +type 字段；T1/T2 奇偶交替防同型疲劳，T1/T3 同向仅感官通道不同；start_ex 扩展入口注入 quiz_cfg_t：audio_ok T3 探测回调 + QUIZ_F_SAME_PREFIX，P1 四参入口保留为兼容包装 P1 测试零改动）+ T3 听音选义（会话开启时逐题探测 audio_ok——发音设置关/无 cloud_id 且无 audio/SD 缺文件均降级 T1，QUIZ_DESIGN 开放问题 1 定案；首题/换题随帧 audio_play_file 异步自动播，不画单词与音标防泄题；中键重播缺源短震 speak 同款反馈）+ 同首字母干扰（quiz_text_fn 新增 slot 2 前缀语义：text 首个 UTF-8 码点，英文首字母/CJK 首字全科目泛化；核心拷贝题干前缀再查候选避免回调静态缓冲覆写，干扰项三阶段：A 同前缀+文本去重→B 文本去重→C 索引兜底，小组耗尽自然回退不阻断出题）；② 2×2 方向直选快答（ui_draw_quiz_cell 网格版式：上=左上/右=右上/下=左下/左=右下方向键即答省中键；T3 恒直选——四向四选项天然配套中键留给重播，T1/T2 由设置「测验快答」开关控制 set_quizgrid 默认关=纵列基线，真机对比后定夺；反馈态同语义迁移：对=选中格高亮保留/错拍1 正确格反白+错格×/拍2 ×保留）；③ UI 题型三态（T2 释义题干 UI_MEAN_LEVEL ≤2 行；P1 恒 T1 遗留补齐）；构建态：native 19 用例 16 绿 + 3 skipped（新增题型轮换/同首字母命中与小组回退/slot2 NULL 静默三用例）、inkword-s3 绿（Flash 78.2%）；真机验收（T3 播音闭环/2×2 方向手感/纵列↔网格对比）待硬件。T5.2 判断+配对题型：判断题 QUIZ_TF（题型枚举扩展三件套——渲染：恒纵列两选项「错/对」固定文案 + 题干单词大字与待判释义行双层（音标略去视觉降噪）；作答：槽位语义复用 answer 0=错/1=对 + sel==answer 判定，quiz_session_answer 零改动，左=错/右=对直答两模式均启用，上/下两槽循环 + 中重播（听音复核）；评分映射：无改动 quality 4/1）+ 真假构造（answer=1 释义来自题词 / answer=0 来自文本互异干扰词，同义库假陈述无解时兜底全真——正确性齐套优先不论难度，不出错题红线）+ 轮换注入（QUIZ_F_TRUE_FALSE flag，i%4==3 槽位每 4 题一道 10 题轮约 2 道，与同首字母 flag 共存不干扰）；配对题零代码确认（T1/T2 本质即四选一配对：化学式↔名称/问题↔答案/词↔释义同构，qa 卡组 front/back 映射 slot 0/1 即配对语义，不另设枚举，QUIZ_DESIGN §2 定案）；native 新增 test_quiz_true_false（flag 开关轮换位/真假构造契约/作答映射/同义库兜底全真/双 flag 共存五段断言）；构建态：native 20 用例 17 绿 + 3 skipped、inkword-s3 绿；真机验收（判断题陈述阅读节奏/左右直答误触率）待硬件。T5.3 App 二期卡组编辑器（[ACCOUNT_MODEL_DECISION](ACCOUNT_MODEL_DECISION.md) §四实施）：后端轻账户——Account 实体（Username 唯一索引 + PBKDF2 密码哈希复用 AuthController 同款实现）+ Deck.OwnerId Guid?（null=官方，索引 + 软删过滤）+ t53Sql 幂等迁移（CREATE IF NOT EXISTS 建表索引 + ADD COLUMN IF NOT EXISTS）；AuthController.IssueToken/VerifyPassword 提 public static（accountId 非空加 NameIdentifier claim；role=learner 与管理端 Admin/Operator 分流，5 个 Admin 控制器补 Roles 白名单防 learner token 越权）；AccountController（register 用户名 2-64/密码≥6 位 + 409 冲突/login/me）+ MyDeckController api/me 卡组 CRUD 全套（payloadType 白名单 word/qa/poem-card；Code="u"+Guid 6 位冲突重试 5 次，≤7 字符设备 deck id 约束同源；条目落 Words 表 Tag=deck.Code 与官方库 uq(Text,Tag) 隔离 + Front/Back 双写 T4.1 契约 + Version 接 max 递增设备增量同步通道天然复用；删除=条目 Archived+Version++ / Deck 软删）；App——cloud_client（ApiResponse {code,message,data} 统一解包 + CloudDeck/CloudSubject）+ account_controller（restore 启动 me() 静默校验 token 过期自动登出；baseUrl 家长可配持久化默认 localhost:5228）+ 编辑器三页（editor 主页登录注册卡/新建科目×版式三选/我的卡组与本地草稿双列离线优先；detail 条目 CRUD 即时写云 + CSV 导入 + LAN 推送；item_sheet 按版式动态字段表单）+ deck_codec（RFC 4180 子集状态机：双引号包裹/""转义/字段内逗号换行/BOM 容错；表头智能跳过中英文标记；buildWordsJson 设备 words.json 契约，qa/poem 语义泛化 text=题面/上句 meaning=答案/下句同 T4.4）+ deck_page 编辑器入口启用（v1.4 预留位兑现）+ main.dart MultiProvider 三层注入；固件 lan_display_server upload +type query 版式白名单校验（非法 400 防拼错静默降级 word-card，不传缺省向后兼容 T3.4）+ deck_manager_upsert 五参透传 manifest 登记版式（L876 预留注释兑现非协议破坏，设备零账户感知红线保持）；构建态：后端 sln 0 错 + 48 测试全绿（AccountTests 9 新增：PBKDF2 哈希/JWT claims ClaimTypes 长名断言——JwtSecurityToken 直构不走出站映射/FillWord 映射）、Flutter analyze 0 issue + 22 测试全绿（deck_codec_test 10 新增：CSV 状态机/引号转义/BOM/表头跳过/maxRows/设备契约）、inkword-s3 绿（Flash 78.2%）；已知缺口：Web 层集成测试（现有范式纯服务直测无认证管线 fixture，端点集成留真机/swagger 验证）；真机验收（注册登录→建组→CSV 导入→LAN 推送→设备切组学习全流程）待硬件。T5.4 AI 批量生成卡组：AiContentService kind=3 DeckItems（WordAiSuggestion record 扩展 Front/Back/Phonetic/Meaning 可选参，旧 4 字段载荷反序列化 null 向后兼容）+ GenerateDeckItemsAsync（科目×版式双维 prompt 三模板：课文→默写卡名句对附拼音译文/知识点→问答卡/单词清单→单词卡；Temperature 0.4；Redis 缓存键 deckId:素材 SHA256 前 12 位:limit TTL 7 天同素材重触发零 token；单次上限 40 条/素材 2 万字符护栏；ExtractJsonArray 数组提取 + markdown 栅栏剥离；长度红线对齐 word_parser.h——Front/Phonetic 60B/Back 250B 同源常量字符边界安全截断）+ DeckGenJob 占位行方案（Text=Front 题面满足 uq(Text,Tag) NOT NULL、Version=0 使 GetIncrementalAsync Version>cursor 恒 false——审校前设备增量同步与全量导出双不可见，防 LLM 幻觉污染词库红线；AiStatus=1 复用既有审校管线；同 Deck 既有 front 含归档跳过防唯一索引冲突；素材级失败仅日志无行可标）+ AdminDeckController GET 列表（弹窗下拉含条目计数）/POST ai-generate 入队 + AdminWordController ai-apply kind=3 分支（ApplyDeckSuggestion 按目标卡组版式分派：poem 对齐 T4.4 云通道契约 text=下句/root=上句/phonetic=拼音/meaning=译文+PayloadJson {"prev":上句}，word/qa 对齐 T4.3 text=题面/meaning=答案；req 人工终值优先于建议原值）+ Version=max+1 增量下发（审校通过即走既有通道，设备零改动红线保持）；管理端：deck-generate-dialog 弹窗（卡组下拉按版式提示素材形态/素材 textarea/条数上限）+ 词库页「AI 生成卡组」入口 + ai-review kind=3 四字段审校卡（题面/拼音/译文只读 + 背面可编辑 req.back 终值）；构建态：后端 sln 0 错 + 60 测试全绿（DeckGenTests 12 新增：三版式解析归一/去重跳过/超长截断/T44·T43 双映射/req 优先/旧载荷兼容/ExtractJsonArray）、Angular ng build 绿；真机验收（素材→生成→审校→设备增量同步→切组学习闭环）待硬件。T5.5 学习策略（纯设备本地闭环，零后端/云改动）：①科目级每日配额——daily_plan goal 键按卡组分派兑现 v1.2 预留（默认组 set_daily 兼容既有键位、其余 sd_<id> ≤3+7=10 键名 15 上限内 lr_st_<id> 同先例；goal_deck(id) 任意组查询 + goal() 活跃组薄壳签名不变设置/概况页零改动）；learning_state 新增 SD01 按组今日小表（NVS lr_stdeck：{deck[8],new,rev}×6 条目 LRU 满 6 覆盖今日总量最小组，跨日清零与 stats_roll 同日同步，评分路径双表同脏同窗口落盘；lr_stats ST01 全局跨组累计 T4.2 决策保留不劫持）+ deck_today_new(deck_id) 查询现取日期比对 ymd（午夜后未评分返回 0 正确语义不依赖 roll 已触发）；daily_plan_done 分子改按组口径防跨组虚达标；②跨科目统计/菜单角标——词书选择页右侧徽标由词条数改为各组今日 n/goal（纯 ASCII TINY 可显，切换前看哪组没学完价值高于词条数）+ 概况页今日进度行分子同改按组；③考试倒计时——daily_plan 考试域三件套（NVS u32 set_exam 存绝对目标日 ymd 而非天数，自治钟重启不失真；设置以「N 天后」表达免三段日期编辑，exam_set_days 时钟未同步拒写；days_left=Howard Hinnant civil 差已过/未设/未同步归 0；urgent ≤7 天窗口）+ 设置页第 6 行（关→1→…→99→关 循环）+ 概况页第 5 行倒计时显示 + due_horizon 注入式放宽（learning_state_set_due_horizon 静态保持切组不丢，due_now 判定时刻前推 horizon 天——考前将到期词提前入队日期反推优先清账，8 天外不提前不打乱 FSRS 节奏；main.cpp 启动/切书两处按 exam_urgent 注入）+ SD01 前向声明/宏前置等编译序修正；构建态：native 34 用例 31 绿 + 3 skipped（test_daily_plan 14 新增：键分派互不串/库外回落/钳位步进/活跃组写入隔离/done 按组分子/考试设置与过期归零/紧急边界 7 天/未同步拒写/跨月与闰年 civil 数学；stubs/nvs.h 内存 kv mock + 时钟/组 id/learning_state 桩）、inkword-s3 绿（Flash 78.3% +0.1%）；真机验收（多组配额切换学习/角标刷新/倒计时到 urgent 后到期队列放宽）待硬件 |
