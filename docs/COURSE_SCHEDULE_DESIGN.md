# 课程表方案设计文档

> **版本** | v0.1（2026-09-06 初稿）
> **定位** | InkWord 多科目周计划编排与自动激活——让用户「不用想今天学什么」
> **依赖** | v1.3 deck_manager（多卡组）+ v1.5 T5.5（科目级配额/考试倒计时）
> **红线** | 课程表是**推荐/自动激活**，非强制锁定；用户随时手动切组不受干预

---

## 一、需求概述

### 1.1 用户场景

学生同时备考多科目（如：英语 CET-4 + 语文古诗 + 化学方程式），每天希望按固定节奏轮换学习，但又不想每次手动切卡组、记配额。

### 1.2 核心诉求

| # | 诉求 | 说明 |
|:--|:--|:--|
| 1 | **周计划编排** | 按星期几配置今日学哪些科目、每科多少词 |
| 2 | **自动激活** | 每天首次使用时自动切到推荐卡组并注入目标量 |
| 3 | **非强制** | 自动激活后用户可手动切组，当日不再自动干预 |
| 4 | **独立配额** | 每个槽位可设独立每日目标量（覆盖卡组全局 goal） |
| 5 | **考试倒计时** | 每个槽位可关联考试日期（覆盖全局 exam） |

### 1.3 与现有模块关系

```
课程表（schedule）
  ├── 读 → deck_manager（卡组清单、id 合法性校验）
  ├── 写 → deck_flow_switch（切卡组编排）
  ├── 覆盖 → daily_plan_goal_deck（槽位配额覆盖卡组全局 goal）
  ├── 覆盖 → exam_set_days / exam_ymd（槽位考试日期覆盖全局）
  └── 触发 → learning_state 评分路径（跨日首次评分触发自动激活）
```

---

## 二、数据模型

### 2.1 槽位结构（schedule_slot_t）

每天最多 4 个槽位（4 个科目时段），7 天 × 4 槽 = 28 个配置单元。

```c
#define SCHED_DAYS   7      /* 周一~周日 */
#define SCHED_SLOTS  4      /* 每日最多 4 个时段 */

typedef struct {
    char  deck_id[8];       /* 卡组短 id（""=空槽不启用） */
    uint8_t goal;           /* 该槽每日目标量（0=使用卡组全局 goal） */
    uint16_t exam_ymd;      /* 考试日期 yyyymmdd（0=使用全局/未设） */
} schedule_slot_t;

/* 完整周配置 */
typedef struct {
    bool enabled;                                    /* 课程表总开关 */
    schedule_slot_t day[SCHED_DAYS][SCHED_SLOTS];    /* 7×4 槽位表 */
    char label[SCHED_DAYS][16];                      /* 每天显示标签（可选，空=默认"周一"等） */
} schedule_cfg_t;
```

**容量分析**：
- 单槽 8+1+2 = 11B，28 槽 = 308B
- 标签 7×16 = 112B
- 总计 ~420B + 开关 1B ≈ **421B**
- NVS blob 单键上限 ~500B 余量充足，**一键存完整周配置**

### 2.2 NVS 键设计

遵循 settings_keys.h 纪律（≤15 字符）：

| 键名 | 类型 | 说明 |
|:--|:--|:--|
| `sched_cfg` | blob | schedule_cfg_t 完整周配置（~421B） |
| `sched_last` | blob | 自动激活状态（见 §3.2） |

仅 2 个新键，不引入键族——课程表是全局单例配置，无需按卡组分派。

### 2.3 自动激活状态（sched_state_t）

```c
typedef struct {
    int32_t last_ymd;       /* 上次自动激活日期（跨日判据） */
    int8_t  last_slot;      /* 上次激活的槽位索引 0~3（-1=无有效槽位） */
    char    last_deck[8];   /* 上次激活的卡组 id（手动干预判据） */
    bool    manual_override;/* 当日已手动切组（true=今日不再自动干预） */
} sched_state_t;
```

---

## 三、核心逻辑

### 3.1 自动激活触发时机

遵循「**仅跨日首次触发**」原则（与 memory 决策一致）：

```
触发点 1：每日首次评分（learning_state_score 路径）
  → stats_roll 检测到跨日 → 调用 schedule_try_activate()

触发点 2：深睡唤醒跨日（power_manager wake 路径）
  → RTC 恢复后检测日期变化 → 调用 schedule_try_activate()
```

### 3.2 自动激活算法

```c
/**
 * @brief 尝试自动激活课程表推荐卡组。
 * @return 0=已激活（切组完成）；1=无需激活（同日/已手动/未启用）；-1=无有效槽位
 */
int schedule_try_activate(void)
{
    /* 1. 检查课程表是否启用 */
    if (!s_cfg.enabled) return 1;

    /* 2. 获取当前日期 */
    int32_t today = epoch_ymd(standby_time_now());
    if (today == 0) return 1;  /* 时钟未同步，不激活 */

    /* 3. 同日不重复触发 */
    if (today == s_state.last_ymd) return 1;

    /* 4. 手动干预后当日不再自动切换 */
    if (s_state.manual_override) {
        s_state.last_ymd = today;  /* 更新日期但不切组 */
        return 1;
    }

    /* 5. 获取今天星期几（0=周一~6=周日） */
    int wday = epoch_wday(today);

    /* 6. 遍历今日槽位，找首个未达标项 */
    for (int s = 0; s < SCHED_SLOTS; s++) {
        schedule_slot_t *slot = &s_cfg.day[wday][s];
        if (!slot->deck_id[0]) continue;  /* 空槽跳过 */

        /* 检查该卡组今日是否已达标 */
        int done = deck_today_new(slot->deck_id);
        int goal = slot->goal > 0 ? slot->goal
                                  : daily_plan_goal_deck(slot->deck_id);
        if (done >= goal) continue;  /* 已达标，看下一槽 */

        /* 找到首个未达标槽位 → 切换卡组 */
        int idx = deck_manager_find_index(slot->deck_id);
        if (idx < 0) continue;  /* 卡组不存在（manifest 变更），跳过 */

        deck_flow_switch(idx);

        /* 注入槽位级配额（覆盖全局 goal） */
        if (slot->goal > 0)
            daily_plan_set_goal_deck(slot->deck_id, slot->goal);

        /* 注入槽位级考试日期（覆盖全局） */
        if (slot->exam_ymd > 0)
            exam_set_ymd_override(slot->exam_ymd);

        /* 记录激活状态 */
        s_state.last_ymd = today;
        s_state.last_slot = s;
        snprintf(s_state.last_deck, sizeof(s_state.last_deck),
                 "%s", slot->deck_id);
        s_state.manual_override = false;
        sched_state_save();

        LOG_I("schedule activated: day=%d slot=%d deck=%s goal=%d",
              wday, s, slot->deck_id, goal);
        return 0;
    }

    /* 所有槽位均已达标 */
    s_state.last_ymd = today;
    s_state.last_slot = -1;
    sched_state_save();
    return -1;
}
```

### 3.3 手动干预检测

用户在自动激活后手动切组时，标记当日不再自动干预：

```c
/* 在 deck_flow_switch 编排中调用 */
void schedule_on_manual_switch(void)
{
    int32_t today = epoch_ymd(standby_time_now());
    if (today == s_state.last_ymd && s_state.last_slot >= 0) {
        /* 同日且今日有过自动激活 → 标记手动干预 */
        s_state.manual_override = true;
        sched_state_save();
        LOG_I("schedule: manual override activated for today");
    }
}
```

### 3.4 跨日重置

每日首次触发时（stats_roll 同路径），重置 manual_override：

```c
/* 在 schedule_try_activate 跨日分支中 */
if (today != s_state.last_ymd) {
    s_state.manual_override = false;  /* 新一天重置手动干预标记 */
}
```

---

## 四、UI 设计

### 4.1 课程表设置页（菜单入口：[学习] 组 → 「课程表」）

**一级页：课程表总览**

```
┌──────────────────────────────┐
│  课程表  [开/关]              │
│                              │
│  周一: 英语 20词 | 古诗 5首   │
│  周二: 化学 15词 | 英语 20词  │
│  周三: 英语 20词              │
│  周四: 古诗 5首 | 化学 15词   │
│  周五: 英语 20词 | 英语 20词  │
│  周六: （休息）               │
│  周日: 古诗 10首              │
│                              │
│  [SET] 编辑  [中] 今日详情    │
└──────────────────────────────┘
```

- 总开关：SET 短按切换 开/关
- 每日行：显示该日有效槽位（卡组名 + 目标量）
- 空日显示「（休息）」
- TINY 档降级：仅显示卡组名首字 + 目标量

**二级页：编辑某天槽位**

```
┌──────────────────────────────┐
│  编辑 周一                    │
│                              │
│  槽1: 英语    20词  考试--    │
│  槽2: 古诗     5首  考试--    │
│  槽3: （空）                  │
│  槽4: （空）                  │
│                              │
│  [↑↓] 选槽  [中] 编辑        │
└──────────────────────────────┘
```

**三级页：编辑单槽位**

```
┌──────────────────────────────┐
│  编辑 周一 槽1                │
│                              │
│  卡组: 英语    ▶ 选择卡组     │
│  目标: 20词    ▶ ±5 调整     │
│  考试: 未设置  ▶ N天后        │
│                              │
│  [↑↓] 选行  [中] 修改        │
└──────────────────────────────┘
```

### 4.2 今日课程表指示（主界面增强）

在主界面/概况页增加一行课程表提示：

```
今日计划: 英语 20词 → 古诗 5首
当前: 英语（已完成 12/20）
下一: 古诗（待开始）
```

### 4.3 菜单入口

在 [学习] 组中插入「课程表」项（位于「词书选择」下方）：

```c
/* menu_ui.c s_items[] 追加 */
{ "── 学习 ──",  NULL, false },       /* 组头（已有） */
{ "📖 开始学习",  act_study, ... },    /* 已有 */
{ "📚 词书选择",  act_deck_sel, ... }, /* 已有 */
{ "📅 课程表",    act_schedule, ... }, /* 新增 */
```

---

## 五、模块接口设计

### 5.1 schedule.h

```c
#ifndef INKWORD_SCHEDULE_H
#define INKWORD_SCHEDULE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCHED_DAYS   7
#define SCHED_SLOTS  4

/** 课程表槽位 */
typedef struct {
    char     deck_id[8];   /* 卡组 id（""=空槽） */
    uint8_t  goal;         /* 目标量（0=用卡组全局） */
    uint16_t exam_ymd;     /* 考试日期（0=用全局） */
} schedule_slot_t;

/** 课程表完整配置 */
typedef struct {
    bool            enabled;
    schedule_slot_t day[SCHED_DAYS][SCHED_SLOTS];
    char            label[SCHED_DAYS][16];
} schedule_cfg_t;

/* ---- 生命周期 ---- */

/** 加载配置（setup 调用一次） */
void schedule_init(void);

/* ---- 配置读写 ---- */

/** 获取配置指针（只读） */
const schedule_cfg_t *schedule_cfg(void);

/** 获取可写配置指针（修改后需调 save） */
schedule_cfg_t *schedule_cfg_mut(void);

/** 持久化配置到 NVS */
void schedule_save(void);

/* ---- 自动激活 ---- */

/**
 * @brief 尝试自动激活（跨日首次调用生效）。
 * @return 0=已激活；1=无需激活；-1=无有效槽位
 */
int schedule_try_activate(void);

/** 手动切组通知（deck_flow_switch 编排中调用） */
void schedule_on_manual_switch(void);

/* ---- 查询 ---- */

/** 获取今日指定槽位（NULL=空槽） */
const schedule_slot_t *schedule_today_slot(int slot_idx);

/** 今日有效槽位数 */
int schedule_today_slot_count(void);

/** 课程表是否启用 */
bool schedule_is_enabled(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_SCHEDULE_H */
```

### 5.2 接线点

| 调用方 | 接线函数 | 时机 |
|:--|:--|:--|
| main.cpp setup | `schedule_init()` | 启动时装载配置 |
| learning_state_score | `schedule_try_activate()` | 评分路径跨日时（stats_roll 后） |
| power_manager wake | `schedule_try_activate()` | 深睡唤醒跨日时 |
| main.cpp deck_flow_switch | `schedule_on_manual_switch()` | 切组编排中 |
| menu_ui.c | `schedule_cfg()` / `schedule_cfg_mut()` | 课程表设置页渲染与编辑 |

---

## 六、NVS 存储策略

### 6.1 键登记（settings_keys.h 追加）

```c
/* ---- 课程表（schedule） ---- */
#define NVS_KEY_SCHED_CFG    "sched_cfg"    /* 周配置 blob（schedule_cfg_t） */
#define NVS_KEY_SCHED_STATE  "sched_last"   /* 激活状态 blob（sched_state_t） */
```

### 6.2 默认值与容错

- **首次启动**：NVS 无键 → `enabled=false`，全槽空 → 课程表关闭，开箱行为不变
- **卡组删除后**：槽位引用的 deck_id 在 manifest 中不存在 → 自动激活时跳过该槽（不崩溃、不提示）
- **时钟未同步**：`epoch_ymd()` 返回 0 → 拒绝激活（与 exam_set_days 同策略）
- **配置损坏**：blob 长度不匹配 → 退化为默认空配置

---

## 七、与现有功能的交互

### 7.1 与 deck_flow_switch 的关系

课程表自动激活**复用** deck_flow_switch 完整编排链路：
- 词库 load + learning_state_reload + reader scope + 渲染刷新
- 不新增切组路径，零重复代码

### 7.2 与 daily_plan 的关系

| 维度 | daily_plan（现有） | schedule（新增） |
|:--|:--|:--|
| 粒度 | 卡组级全局 goal | 槽位级时段 goal |
| 优先级 | 基线 | 覆盖（槽位 goal > 卡组全局 goal） |
| 考试 | 全局单个 exam | 每槽可设独立 exam |
| 生效方式 | 手动设置 | 自动注入 |

**冲突解决**：槽位 goal 写入时，仅影响该槽位对应卡组。用户手动切到其他卡组后，读取的是该卡组的全局 goal（不受槽位影响）。

### 7.3 与 learning_state dstats 的关系

- 达标判据读取 `deck_today_new(deck_id)`（dstats 按组统计）
- 课程表不修改 dstats 结构，仅消费其数据

### 7.4 与 stats_roll / dstats_roll 的关系

- `schedule_try_activate()` 在 `stats_roll()` / `dstats_roll()` 完成后调用
- 跨日判据复用同一日期比对逻辑

---

## 八、实施计划

### 8.1 分步实施（约 3-4 天）

| 步骤 | 内容 | 估时 |
|:--|:--|:--|
| S1 | schedule.h/c 骨架：数据结构 + NVS 读写 + init | 0.5d |
| S2 | 自动激活核心：try_activate + manual_override + 跨日重置 | 0.5d |
| S3 | 接线：main.cpp setup + learning_state 评分路径 + power_manager wake | 0.5d |
| S4 | 课程表设置 UI：一级总览 + 二级编辑天 + 三级编辑槽 | 1d |
| S5 | 主界面今日提示 + 菜单入口 | 0.5d |
| S6 | native 单测：配置读写/自动激活/手动干预/跨日/空槽/卡组缺失 | 0.5d |

### 8.2 测试计划

| 层 | 手段 |
|:--|:--|
| native 单测 | 配置序列化/默认值/自动激活算法/手动干预标记/跨日重置/卡组缺失跳过/时钟未同步拒绝 |
| 构建 | `pio run` 全 env 零警告 |
| 实机 | 配置 3 科目周计划 → 跨日自动切组 → 手动切组后当日不再自动切 → 次日恢复 |

---

## 九、被拒绝的替代方案

1. **每日单槽（简化版）**——只支持每天一个科目，不够灵活（学生可能上午英语下午古诗）；4 槽开销仅多 33B/天，值得投入
2. **云端同步课程表**——课程表是纯本地习惯配置，不同设备可能不同安排；同步增加协议复杂度但价值低
3. **强制锁定模式**——锁定用户不让切组违背「非强制」原则，且与现有 deck_flow_switch 手动切组体验冲突
4. **独立 NVS 键族（每槽一键）**——28 槽 = 28 键，NVS 键名 15 字符限制下拼接复杂且占用键空间；单 blob 一键更简洁
5. **App 端编辑课程表**——一期设备端菜单编辑够用（7×4 矩阵虽深但操作频次低）；App 端编辑随 v2.0 配置同步一并考虑

---

## 十、涉及文件清单

**新增**：
- `InkWord_Firmware/src/schedule.h` — 接口定义
- `InkWord_Firmware/src/schedule.c` — 实现

**修改**：
- `settings_keys.h` — 新增 2 个 NVS 键
- `main.cpp` — setup 调 init + deck_flow_switch 调 manual_switch
- `learning_state.c` — 评分路径跨日调 try_activate
- `power_manager.c` — 深睡唤醒跨日调 try_activate
- `menu_ui.c` — 课程表设置页（一级/二级/三级）+ 菜单入口
- `CMakeLists.txt` — 新增 schedule.c
- `deck_manager.h` — 新增 `deck_manager_find_index()` 声明（id → idx 反查）
