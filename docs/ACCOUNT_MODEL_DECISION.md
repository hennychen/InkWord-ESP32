# 账户体系分层决策（ADR-001）

> **状态** | 定稿（2026-08-24，v1.4 T4.6 决策不实施）；v1.5 §四已实施（2026-08-25，T5.3 交付：Account/OwnerId/注册登录/卡组 CRUD/App 编辑器/固件零改动红线均兑现）
> **决策** | **v1.5 App 轻账户只管内容归属 / v2.0 完整 UserId + 多设备**，LearningRecord 挂 DeviceId 现状两个版本均不动
> **动因** | G6 账户决策自 v2.0 前移 v1.4（ROADMAP §一）：v1.5 卡组编辑器（T5.3）即产生 UGC 内容，归属缺位则换设备丢卡组、无多设备分享，UGC 生态地基须先行；完整账户（多设备同步）交互与迁移成本超出 v1.5 窗口
> **勾稽** | ROADMAP §五 #6 / §六 #2；PRD_GAP §四「设备即用户」差异 + §九 6「决策学习者账户体系」；PRD §6.2 数据表

---

## 一、现状基线（2026-08-24 代码事实）

| 资产 | 现状 | 本决策处置 |
|:---|:---|:---|
| [User.cs](../InkWord_Backend/src/InkWord.Core/Entities/User.cs) | **管理后台账号**（Username/Role，AuthController JWT 颁发） | 语义保持「后台操作员」，与学习者账户**分表**（见 §四） |
| [Device.cs](../InkWord_Backend/src/InkWord.Core/Entities/Device.cs) | `Guid? UserId` 可空列已预留（未使用）；ApiKey=X-Device-Key 设备鉴权 | **v2.0 兑现**（设备↔账户绑定）；v1.5 不动 |
| [LearningRecord.cs](../InkWord_Backend/src/InkWord.Core/Entities/LearningRecord.cs) | 挂 **DeviceId**（FSRS 影子：Stability/Difficulty/NextReview + 错词/收藏） | **两版均不改列**——多设备同步走聚合层（见 §五） |
| 固件 sync_client | 设备侧零账户概念（MAC 注册 + ApiKey） | **v1.5 零改动红线**（轻账户只活在 App/云端） |
| Deck（T4.1） | Subject/Deck/Item 三表，Deck 无 Owner | **v1.5 加 `OwnerId Guid?`**（null=官方/公共） |

## 二、方案对比

| 方案 | 内容 | 结论 |
|:---|:---|:---|
| **A. 维持「设备即用户」至 v2.0** | 不建账户，卡组全走 SD 拷贝/LAN 直传，无云端归属 | ❌ 否决：v1.5 编辑器产出的 UGC 无归属即无法云备份/换机恢复/多设备分发，事后补列 + 历史回填成本高于先建；「设备丢失=内容丢失」对自制内容不可接受 |
| **B. v1.5 轻账户 + v2.0 完整（采纳）** | v1.5 只管内容归属；学习记录仍挂设备；v2.0 绑定 + 聚合同步 | ✅ 分层与刚需对齐：v1.5 唯一刚需是归属（B 全覆盖），多设备同步的交互/迁移成本留 v2.0 专职窗口 |
| **C. v1.5 直接完整账户** | 一步到位 UserId 挂 LearningRecord + 设备绑定 UI | ❌ 否决：墨水屏设备绑定交互成本高（五向键输凭据不现实，须 App 侧绑定流程）；LearningRecord 迁移破坏 FSRS 双端一致红线；超出「体验版」工期 |

## 三、不变量（红线，两版共用）

1. **离线优先**：无账户 = 全部学习功能可用（设备本地闭环，FSRS/错词/收藏/卡组 LAN 推送均不依赖账户）；账户只增益不设卡。
2. **LearningRecord.DeviceId 不动**：学习状态以设备为调度主体（固件 SRS 引擎本地运转，云端影子），任何同步语义都不改此列——多设备在**聚合层**解决（§五）。
3. **FSRS 双端一致**：账户层不得介入评分/排期链路（LearningRecord 仍是设备的影子，不因账户合并而合并状态）。
4. **协议兼容**：v1.5 设备固件零改动（轻账户不触设备协议）；v2.0 设备绑定走 App 侧完成，设备端仅消费绑定结果（ApiKey 换发）。
5. **管理账号与学习者账号分表**：后台操作员（RBAC/审计）与 App 学习者（内容归属）安全等级与生命周期不同，混表互相污染——新实体 `Account`，不复用 `User`。

## 四、v1.5 轻账户设计边界（T5.3 实施依据）

- **管**：App 注册/登录（复用 JWT 基建，独立签发路径 role=learner）→ 自建卡组云端归属与备份（Deck.`OwnerId`）→ 账号内多设备**分发内容**（同一账号的设备清单可见/推送卡组，仍是内容语义）。
- **不管**：学习记录归属（仍挂 DeviceId）、跨设备进度漫游、付费/会员（v2.0+ 生态）、儿童实名合规（单机学习设备 + 家长后台账号合一，暂无强监管触发）。
- **实体**：`Account(Id, Username, PasswordHash(PBKDF2 同 AuthController 既有实现), CreatedAt)` + `Deck.OwnerId Guid?`（null=官方）；Account↔Device 不建关联（v2.0 经 Device.UserId 兑现）。
- **设备侧零感知**：卡组到设备仍走 T3.4 LAN 直传/增量同步，设备不知道卡组属于谁。

## 五、v2.0 完整账户边界（实施方向，届时可修订）

- **绑定**：App 登录后扫码/输 DeviceKey 绑定 → 写 `Device.UserId`（既有可空列兑现）+ 设备 ApiKey 换发（设备重注册语义，MAC 幂等先例沿用）。
- **多设备同步 = 聚合不迁移**：按 `(UserId, WordId)` 聚合多设备 LearningRecord，**Last-Write-Wins**（LastStudiedAt 新者胜，对应 ROADMAP §七「LWS 策略已有」）；设备拉取时以聚合胜者覆盖本地影子——固件仍是本地调度主体，云端聚合只影响下次同步基准。
- **深度进度漫游（跨设备续学）**：默认「各设备独立学习，聚合视图只读」；强制续学（一台学一半换另一台继续 FSRS 队列）为 v2.x+ 增强项，不在 v2.0 承诺。
- **生态**：卡组商店/UGC 分享/付费挂 Account（依赖 v1.5 归属地基 + v2.0 绑定）。

## 六、实施影响速查

| 版本 | 后端 | App | 固件 |
|:---|:---|:---|:---|
| v1.5（T5.3） | +Account 表/注册登录端点；Deck+OwnerId；卡组 CRUD 挂归属 | 登录页 + 我的卡组 + 推送（LAN 复用） | **零改动** |
| v2.0 | Device.UserId 兑现 + 绑定端点 + LWS 聚合同步服务 | 设备管理/绑定流程 | ApiKey 换发消费（sync_client 既有重注册链路微调） |

## 七、决策依据存档

- 全科化方向分析 G6（2026-08-24）：自定义卡组归属需要 → 决策前移 v1.4、实施分层 v1.5/v2.0；
- PRD_GAP §四：PRD Users（Email/DailyNewWordGoal）vs 实际「设备即用户」语义级差异——本决策将差异显式分层而非一次性抹平；
- 墨水屏交互成本：无键盘输入，任何设备侧凭据交互都不现实，绑定必须经 App；
- 同类产品先例：单词类硬件（如词汇卡片机）普遍「设备即用户 + App 账户管内容」，账号直接挂学习进度会显著抬高售后（换机迁移纠纷）。
