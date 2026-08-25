# 快捷菜单（menu_ui）设计文档

| | |
|---|---|
| **版本** | v1.3（已实施；v1.2 增补按键说明页，v1.3 增分组标题行 + INFO 页分页今日统计） |
| **日期** | 2026-08-24 |
| **状态** | ✅ 已实施：8 env 构建零新警告 + native-test 通过；实施与设计的差异注记见 §6.3/§7.2/§7.3；真机回归清单见 §10 验收标准 |
| **背景会话** | 3.7" 屏真机测试期交互分析（2407 词内嵌词库已就绪） |

---

## 一、背景与动机

### 1.1 按键资源现状：14 槽位全满

学习页交互矩阵（`main.cpp` `on_button()`，7 键 × 短/长 = 14 槽位零空闲）：

| 键 | 短按 | 长按 |
|---|---|---|
| 上 | 释义翻页 / 上一词 | 清残影全刷 |
| 下 | 释义翻页 / 下一词 | 模式循环（闪卡→听写→复习→阅读，线性盲轮播） |
| 左 | 自评「忘记」Q1 | AP 配网门户 |
| 右 | 自评「简单」Q5 | LAN 接收页 |
| 中 | 发音 | Wi-Fi 配网 |
| SET | 遮蔽/揭晓释义 | 收藏切换（P1 已实现） |
| RST | 回本组第一条 | 错词本进出（P1 已实现） |

仅存富余：待机页短按上/下/左/RST 四槽忽略（但入口藏在待机页，不可发现）。

### 1.2 功能缺口

| 功能 | 状态 | 入口问题 |
|---|---|---|
| **收藏列表浏览** | 半成品 | SET 长按只能加星/取消，收藏了哪些词设备上无法查看——闭环断裂，**最急** |
| 设置页（字号/每日新词量（v1.5 按组）/发音/震动/测验快答/考试倒计时开关） | ✅ v1.2 已实现，v1.5 扩至 6 行 | settings_ui 覆盖层（§7.5），入口 [系统] 组「设置」项 |
| 词书/词库切换 | 未实现（v1.3 Deck 化） | PRD §7.1 规划 SD 多词库 + 云端下发，设备端无入口 |
| 设备信息（版本/IP/电量/存储） | ✅ 已实现（电量行 v1.2 补齐） | §7.3；电量经 MAX17048（I2C 0x36），未在位显示「--」 |
| 模式切换 | 已有但体验一般 | 长按下盲轮播——无预览、不知去哪 |

### 1.3 方案对比

| 方案 | 结论 |
|---|---|
| A. 组合键/双击/超长按 | ❌ 不可发现性为零；五向开关单手组合物理困难；button_handler 需扩展状态机 |
| B. **快捷菜单页** | ✅ 采纳 |
| C. 全推给 App/网页 | ⚠️ 设置类低频可推（管理后台已就绪）；收藏浏览是学习高频，通勤裸机场景必须有设备端入口 |

**TINY 窄屏附带收益**：在途 2.13"/2.9" 竖屏（LAYOUT_TINY 档，122/128px 宽）上列表式菜单天然适配（逐行渲染），比组合键方案对屏幕尺寸鲁棒。现在定范式，新屏直接继承。

## 二、入口与按键语义变更

| 变更点 | 原语义 | 新语义 |
|---|---|---|
| 学习页长按**中** | Wi-Fi 配网直达 | **功能菜单**（配网降为菜单项③） |
| 待机页长按**中** | Wi-Fi 配网直达 | 功能菜单 |
| 长按 **RST** | 错词本进出 | 临时视图进出（错词本 **或** 收藏浏览，见 §6.3） |
| 其余 12 槽位 | — | **不动** |

**设计依据**：配网是低频高流程功能（一次配好终身不改），却占着中键长按黄金位。稀缺席位给高频直达，低频功能收进菜单。清残影/模式循环/收藏/错词本四个学习域高频或急救操作保留直达。

**菜单内按键**（菜单激活时独占，七键全收）：

| 键 | 语义 |
|---|---|
| 上/下 短按 | 移动选择（循环滚动） |
| 中 短按 | 确认/进入 |
| SET 短按 | 返回上级（主菜单=退出菜单） |
| RST 短按 | 退出菜单（任意层级直接回学习页） |
| 长按 | 全部忽略（防误触） |

**风险提示**：中键长按语义变更需同步三处文档（PRD §5.3 总表、WIRING_DIAGRAM 按键表、README），老用户肌肉记忆有一次迁移成本——项目开发期零存量用户，当前是零成本窗口。

## 三、模块架构

```
src/menu_ui.c/.h          ← 新模块（并列 wifi_config_ui，纯 UI + 路由）
src/study_mode_machine.c  ← 扩展：MODE_COLLECTION 临时视图
src/learning_state.c/.h   ← 扩展：collected 枚举 API 一对
src/main.cpp              ← on_button 路由插入 + 两处长按中改道
```

**路由插入点**（`on_button()` 现有顺序）：

```
唤醒吞除 → 【新增 menu_ui_is_active() → menu_ui_on_button(id, event); return】
        → wifi_config_ui → LAN → standby → 长按块 → 短按模式路由
```

菜单置于最前：顶层覆盖层，激活期间接管一切（与 wifi_config_ui 同级语义）。菜单启动子功能（配网/AP/LAN）时**先自我退出**再调既有入口函数（「先 exit 后 enter」纪律），避免双激活。

**复用先例**：
- 列表范式：`wifi_config_ui.c` `draw_list_page()` / `draw_list_body()`（反选高亮/滚动条/局刷重绘，2026-08 已真机验证）
- 序列机制：`study_mode_machine.c` `study_mode_enter_wrongbook()` 临时视图 + `seq_word_index()` 过滤游走（O(N) 枚举，N≤4000 可接受）
- 收藏标志：`learning_state.c` `learning_state_is_collected()`（P1 已实现，`collected` 位已在状态结构）

## 四、菜单 UI 布局

### 4.1 结构（三段式 + 分组标题行）

```
┌─────────────────────────────┐
│ 功能菜单                2/9 │ ← 标题栏（白底黑字，序号=可选项位/9）
├─────────────────────────────┤
│ [ 学习 ]                    │ ← 组头行（16px 小字，不可选中）
│ ▶ 收藏列表               12 │ ← 列表区（反选高亮 = 黑底白字）
│   模式选择            闪卡 │
│   AI 对话                   │
│ [ 同步 ]                    │
│   音频同步          3/2407 │
│   Wi-Fi 配网       已连接  │ ← 左 CJK 标签 + 右徽标
│   ...（滚动，MID 可见 4 行）│
├─────────────────────────────┤
│ 上/下 选择  中 确认  SET 返回│ ← 底部提示栏（TINY 省略）
└─────────────────────────────┘
```

v1.3 分组（百词斩/学习机菜单经验）：**[学习]**（收藏列表/模式选择/AI 对话）、
**[同步]**（音频同步/Wi-Fi 配网/AP 配网门户/LAN 接收页）、**[系统]**（设备
信息/按键说明）。组头行样式同按键说明页组头（方括号），16px 小字永不反选，
光标循环跳过不可停驻；主列表 12 行（3 组头 + 9 项），标题栏序号计数不含组头。

### 4.2 四档几何表（全部运行期派生，`MU_*` 宏，零特判）

| 参数 | 派生公式 | MID 416x240 | SMALL 264x176 | TINY 122x250 | TINY 128x296 |
|---|---|---|---|---|---|
| MU_TITLE_H | 复用 UI_STATUS_H 语义 | 32 | 32 | 24 | 24 |
| MU_ITEM_H | `TINY?28 : SMALL?36 : 44` | 44 | 36 | 28 | 28 |
| MU_HINT_H | `TINY?0 : 22` | 22 | 22 | **0（省略）** | 0 |
| MU_VISIBLE | `(H-标题-提示)/ITEM_H` | 4 | 3 | 7 | 9 |
| MU_ITEM_W | `W - 2*UI_MARGIN_X` | 384 | 232 | 106 | 112 |

- **CJK 标签**：16px 点阵（level 0，全档统一）——FreeSans 无汉字，必须走 `cjk_text` 原语（单行绘制用 `cjk_text_draw_wrap(x, y, 大宽度, 0, ...)` 不触发折行）
- **右侧徽标**：FreeSans size 1 右对齐（`12` / `OK` / 当前模式名 CJK 小字）；TINY 档宽度不足时省略
- **反选高亮**：`fill_rect` 黑 + 文字 `EPD_GFX_WHITE`（`cjk_text_draw_wrap` 与 `epd_gfx_draw_text` 均有 color 参数）
- **滚动条**：右侧 4px 宽滑块（`MU_VISIBLE < 项数` 时显示）

### 4.3 菜单项定义（v1.3 分组化 12 行 = 3 组头 + 9 项）

```c
typedef struct {
    const char *label;                      /* CJK 标签（组头=组名） */
    bool is_header;                         /* v1.3：组头行不可选中 */
    void (*badge)(char *buf, size_t n);     /* 可选：右侧徽标填充 */
    void (*activate)(void);                 /* 中键确认动作 */
} mu_item_t;
```

| # | 组 | label | badge | activate |
|---|---|---|---|---|
| — | 学习 | `[ 学习 ]` | — | （组头） |
| 1 | | 收藏列表 | `n`（collected_count，0 显示「空」） | 进收藏浏览视图（§6.3） |
| 2 | | 模式选择 | 当前模式名 | 进二级列表（§7.2） |
| 3 | | 词书选择 | 当前卡组名（v1.3 增，T3.1） | 进二级词书列表（§7.6） |
| 4 | | AI 对话 | — | 进 MODE_CHAT 临时视图 |
| 5 | | 快速测验 | — | `menu_ui_exit(); quiz_flow_start();`（v1.2 增，QUIZ_DESIGN.md） |
| — | 同步 | `[ 同步 ]` | — | （组头） |
| 5 | | 音频同步 | 缺 N/总 M | 后台串行下载 |
| 6 | | Wi-Fi 配网 | 已连接/未连接 | `menu_ui_exit(); wifi_config_ui_enter();` |
| 7 | | AP 配网门户 | — | `menu_ui_exit(); lan_portal_enter();` |
| 8 | | LAN 接收页 | — | `menu_ui_exit(); lan_server_enter_receive_page();` |
| — | 系统 | `[ 系统 ]` | — | （组头） |
| 9 | | 设置 | — | 进 settings_ui 覆盖层（§7.5，v1.2 增） |
| 10 | | 设备信息 | — | 切 INFO 页（§7.3） |
| 11 | | 按键说明 | — | 切按键说明页（§7.4） |

v1.2 兑现原二期预留两项：设置（§7.5）、快速测验；v1.3 兑现词书选择（§7.6，T3.1 Deck 化）——二期预留全部落地。

**图标前缀（v1.3 视觉升级）**：列表项 20px 1bit 剪影图标（tools/gen_menu_icons.py 几何谓词 + 3×3 过采样生成，menu_icons.h ~660B flash），draw_item 反选时白色剪影；TINY 档省略（项高 28、宽度紧张，同中文徽标先例）。

## 五、状态机（菜单内部）

```
            长按中(学习/待机页)                SET/RST/中(空确认)
  ┌──────┐ ─────────────────→ ┌───────┐ ─────────────────→ ┌──────┐
  │ IDLE │                    │ LIST  │                    │ IDLE │
  └──────┘ ←───────────────── └───────┘ → activate() 派发：
                                     ↑│      ├─ 项1: exit + study_mode_enter_collection()
                              SET 返回│      ├─ 项2: LIST→MODELIST（二级列表，SET 回 LIST）
                                     │      ├─ 项3-5: exit + 既有入口函数
                                     │      └─ 项6: LIST→INFO（信息页，SET 回 LIST）
                                     └ INFO/MODELIST ← SET
```

- **渲染策略**：进入/换页 = 全刷；选择移动 = 清列表区 + 重绘 + `epd_gfx_flush_window_passes(0, MU_TITLE_H, W, H-MU_TITLE_H, 1)` 单遍局刷（抄 `wifi_config_ui.c` `draw_list_page(partial=true)` 路径）
- **局刷保养**：独立计数阈值 10（对齐 `WIFI_UI_PARTIAL_MAX`），达阈值升级全刷
- **退出恢复**：`menu_ui_exit()` → main 侧 `ui_render_current()`（模式可能已变，need_full 自然触发全刷）
- **三色屏**：`partial_enabled=false` 的面板局刷请求自动降级全刷，无需特判

### 与现有路由的互斥

| 场景 | 行为 |
|---|---|
| 菜单激活 + 子功能被拉起 | 菜单先 exit（不再拦按键），子功能走既有路由 |
| 菜单激活 + 深睡到期 | `power_note_activity` 顶部已刷新；若真睡去，唤醒=重启回学习页（菜单为临时层不恢复，与配网页现状一致） |
| 菜单激活 + LAN 帧到达 | LAN 接收页是用户主动进入型，后台帧不抢占（现状不变） |

## 六、收藏浏览视图（MODE_COLLECTION，错词本同构）

复用错词本全套基建，过滤条件从 `consecutive_wrong > 0` 换成 `collected`：

```c
/* learning_state.h 新增（镜像 wrong_count/wrong_at 对） */
int learning_state_collected_count(void);
int learning_state_collected_at(int pos);

/* study_mode_machine 扩展 */
MODE_COLLECTION,   /* 临时视图：收藏词浏览，不入 switch_next 循环，不 NVS 恢复 */
static int seq_word_index(int cursor) {
    return (s_current == MODE_WRONGBOOK)  ? learning_state_wrong_at(cursor)
         : (s_current == MODE_COLLECTION) ? learning_state_collected_at(cursor)
                                          : cursor;
}
bool study_mode_enter_collection(void);   /* 空收藏返回 false（菜单项长震不进入） */
void  study_mode_exit_collection(void);
```

**视图内行为**（全部继承现有键义，零新增）：

| 操作 | 行为 |
|---|---|
| 上/下 | 收藏序列内翻词（含释义分页 `ui_mean_page_step`） |
| 中 | 发音 |
| SET 短按 | 遮蔽/揭晓（闪卡自测） |
| **SET 长按** | 取消收藏 → 当前词移出序列，游标钳位 n-1（**实施注记**：与 `study_mode_after_quality` 的回绕 0 略异——取消末词时显示前一词不跳跃，清空自动退出，见 `study_mode_after_uncollect` 注释） |
| 左/右 | 自评（收藏与掌握正交，不从序列移除） |
| RST 短按 | 回收藏首词 |
| RST 长按 | 退出收藏视图回闪卡 |

**RST 长按路由改三级判**：`WRONGBOOK→退出；COLLECTION→退出；否则→进错词本`。

状态栏显示「收藏」模式名 + `n/总数`（`ui_draw_status` 现成，`study_mode_name` 加一项）。

## 七、子页面设计

### 7.1 收藏列表入口体验

中键点「收藏列表」→ **直接进收藏浏览视图**（词卡逐词浏览，不做中间列表页）。理由：收藏核心动作是翻看词卡，二级列表页多一跳无增益；词卡已含发音/释义/自评全部能力。徽标为 0 时确认长震（HAPTIC_ERROR）不进入。

### 7.2 模式选择（二级列表）

4 项：闪卡/听写/复习/阅读，当前项标记 `●`（**实施注记**：U+25CF 字库覆盖未验证，实施改为光标预定位当前模式行——反选高亮即「当前」标记，信息等价零字库依赖）。中键 → `study_mode_set(mode)`（经 apply_mode 重构承载全副作用：游标归零/NVS 持久化/READER 进度恢复，修复原残缺语义）+ 菜单退出 + 全刷重绘。长按下盲循环保留（高频肌肉记忆路径），本项是可视化补充入口，二者终态一致。

### 7.3 设备信息（INFO 页；v1.3 分页化 + 今日统计；v1.5 T5.5 概况页 5 行）

分页只读页（上/下翻页循环，同按键说明页范式），第 1 页 5 行、第 2 页 5 行 CJK 标签 + 混排值：

```
── 第 1 页「学习概况」──
词库       2407 词
收藏/错词  12 / 3
今日进度   5/20 新 21 复   ← 分子按活跃卡组（v1.5，SD01 表）
连续学习   7 天            ← 每日 ≥1 评分的连续天数（civil_days 差=1 判连续）
考试倒计时 7 天            ← v1.5 增（未设/已过/未同步显「--」）

── 第 2 页「设备信息」──
固件版本   1.0.0
运行时长   02:14:36
电量       88% (3.92V)
IP 地址    192.168.10.192（未连接则「--」）
PSRAM      8.0/4.6 MB
```

数据源：今日进度=`learning_state_deck_today_new(活跃组)`（v1.5 按组分子，NVS lr_stdeck SD01 小表；全局跨组累计 lr_stats 口径保留在 streak/复习计数）/连续学习=`learning_state_streak_days()`（评分时随 FSRS 同窗口落盘，自治钟 epoch 日结，未同步时挂起首个评分补结）；今日行含 "n/goal" 配额进度（v1.2 起，v1.5 分子按组、goal 键按组分派）；考试倒计时=`exam_days_left()`（daily_plan 考试域，NVS u32 `set_exam` 绝对 ymd）；`FW_VERSION` 经 `fw_version()` getter 导出；IP=`wifi_get_sta_ip()`；电量行 v1.2 经 MAX17048 读 SOC（"88% (3.92V)"，模块未在位显示「--」，心跳报文回退 100）；TINY 档超宽值自然截断（bring-up 再调）。

### 7.4 按键说明页（v1.2 增补；v1.3 增复习词表组）

主菜单第 11 项，表驱动分页只读页，六组内容：学习页（含 `*` = 已收藏标记行）/复习词表（2026-08-24 增：列表选择/详情/自评出队）/收藏·错词视图/AI 对话（v1.2 增快速测验组）/待机页/菜单内。行文全半角标点（字库安全先例），「短按 / 长按」两段式。上/下翻页循环、中=下一页、SET 返回主菜单、RST 退出菜单；翻页局刷，标题栏右上页码。TINY 档值列宽不足，bring-up 后改单列两行/键（代码已注记）。

### 7.5 设置页（v1.2 增，settings_ui.c）

主菜单 [系统] 组「设置」项（第 9 项）进入。覆盖层范式三件套（`settings_ui_enter/is_active/on_button`），镜像 menu_ui：无 init、静态零初始化、无阻塞，`on_button` 转发链在 menu_ui 之后。行项六条（v1.2 四条 + T5.1 测验快答 + T5.5 考试倒计时）：

| 行 | 操作（中键） | NVS 键（"inkword" 命名空间） | 默认 |
|---|---|---|---|
| 每日新词量 | +5 循环（5→100→5） | 按组分派：默认组 `set_daily`、其余 `sd_<id>`（u8） | 20 |
| 发音 | 开/关切换 | `set_audio`（u8） | 开 |
| 震动 | 开/关切换 | `set_haptic`（u8） | 开 |
| 字号 | 档循环（关/大字） | `set_font`（u8） | 关 |
| 测验快答 | 开/关切换（v1.5 T5.1，2×2 方向直选） | `set_quizgrid`（u8） | 关 |
| 考试倒计时 | 关→1→…→99→关 循环（v1.5 T5.5，按天数设置） | `set_exam`（u32，存绝对目标日 ymd；0=未设） | 关 |

键缺失=默认（不写默认值）。取值 API 惰性缓存（`settings_audio_enabled()/settings_haptic_enabled()/settings_font_mode()`，int8_t −1→NVS 首读），按键高频路径零 flash 读。门控接线：`haptic_event`/`ui_sfx_play` 入口统一拦截；词条发音在模式机 speak 分支拦截；大字档语义=阅读模式默认字号 +1 档 + 词卡释义排版档派生（TINY/SMALL 屏 0→1、MID+ 屏 1→2）。上/下移动、SET/RST 退出（退出时 `ui_force_font_refresh()` 强制全刷重排当前页）。每日新词量与 daily_plan 模块同源（v1.5 起键按卡组分派、`daily_plan_goal()` 活跃组薄壳，§7.3 今日行 n/goal 同口径）；考试倒计时设置时若自治钟未同步拒写（保持「关」，HTTP 校准后再设）。

### 7.6 词书选择页（v1.3 增，T3.1 Deck 化）

主菜单 [学习] 组第 3 项（menu_icon_decks 图标）。二级列表页镜像模式选择页范式（§7.2）：进入即 `deck_manager_scan()` 重扫（SD 插拔新卡组即所见即所得，幂等），光标预定位活跃卡组，右侧徽标 v1.5 T5.5 起为各组今日 "n/goal"（分子=SD01 按组小表，分母=按组分派 goal 键；纯 ASCII TINY 可显——切换前看「哪组没学完」价值高于 v1.3~v1.4 的词条数徽标），超一屏滑动窗口跟随光标。

数据链：`/sdcard/decks/manifest.json`（`{version, decks:[{id,name,subject,payloadType,file,count}]}`，subject/payloadType 为 v1.4 泛化预留、本版解析即忽略）+ `/sdcard/decks/<id>/words.json`；现 /sdcard/words.json 兼容为默认卡组（恒在列表 [0]，无 manifest 开箱行为不变）。

中键=切换（`deck_flow_switch` 编排：NVS `deck_active` 记录 → 词库三级递降重载（卡组文件→words.json→内嵌）→ `learning_state_reload` 显式作废重锁（词数相同的两本书间不串状态）→ `reader_set_progress_scope` 阅读进度键换卡组后缀（rd_page_&lt;id&gt;，切书不丢进度；默认卡组无后缀零迁移）→ 归位闪卡）；重复选当前卡组轻反馈；文件缺失/重载失败长震拒绝。id ≤7 字符（NVS 键名 15 上限）。

## 八、性能与内存预算

| 项 | 预算 | 说明 |
|---|---|---|
| menu_ui 静态状态 | < 256 B | 状态/游标/局刷计数，无堆分配 |
| 收藏视图 | 0 新分配 | 序列虚游走（collected_at O(N)，N≤4000，与错词本同构已验证可接受） |
| Flash 增量 | ~7 KB | menu_ui + 状态机扩展 |
| 选择移动刷新 | 单遍局刷 ~350ms | 与 wifi_ui 列表滚动同体验（BW 面板） |
| 收藏枚举优化预留 | O(N)/次 | 若滚动卡顿，优化路径=进页时建 uint16 索引缓存（≤8KB PSRAM），一期不做 |

## 九、文档变更清单（实施时同步）

| # | 文档 | 位置 | 变更 |
|---|---|---|---|
| 1 | docs/PRD_V2.1.md | §5.3 按键总表 | 长按中：Wi-Fi 配置→**功能菜单**（全局行+待机页列）；RST 长按：错词本→临时视图进出；新增 §5.7「功能菜单」小节 |
| 2 | docs/WIRING_DIAGRAM.md | 按键表 GPIO21 中键行 | 长按释义改为「功能菜单」，同步日期 |
| 3 | InkWord_Firmware/README.md | 模块表 | 新增 menu_ui 行；交互说明段补菜单一段 |
| 4 | docs/PRD_GAP_ANALYSIS.md | §5.3 冲突表 | 「长按中=收藏与配网冲突」行标记已解决（菜单化重构） |
| 5 | docs/PANEL_COMPAT_DESIGN.md | §8.1（可选） | TINY 版式列补「菜单提示栏省略」注记——也可只在 menu_ui.c 注释 |

## 十、实施任务分解（✅ 2026-08-23 全部完成）

| # | 任务 | 文件 | 量级 |
|---|---|---|---|
| 1 | collected_count / collected_at API | learning_state.c/.h | ~15 行 |
| 2 | MODE_COLLECTION 临时视图（enum/enter/exit/seq 过滤/收缩钳位/name） | study_mode_machine.c/.h + main.cpp RST 长按三级判 + SET 长按收藏收缩 | ~60 行 |
| 3 | menu_ui 模块（列表渲染/按键/二级页/INFO/徽标） | menu_ui.c/.h + CMakeLists | ~400 行（大头） |
| 4 | main.cpp 集成（路由插入+两处长按中改道+standby 转发） | main.cpp + standby_page.c | ~25 行 |
| 5 | IP/电量 getter 补缺 | wifi_manager / main | ~15 行 |
| 6 | 文档同步（§九 表 4 处必改） | 4 个 md | — |
| 7 | 验证：8 env 构建零新警告 → MID 真机全路径回归 → `pio test` | — | — |

**构建命令**（PATH 坑见项目惯例）：

```bash
cd InkWord_Firmware
PATH="$HOME/.platformio/penv/bin:$PATH" pio run -e inkword-s3 -t upload --upload-port /dev/cu.usbserial-0001
PATH="$HOME/.platformio/penv/bin:$PATH" pio test -e native-test
```

**备份纪律**：动工前按项目规范备份待改文件（.bak 序号查空位递增）。

**验收标准**：
- 长按中从学习页/待机页均进菜单；SET/RST 退出后原页面正确恢复（模式变化时全刷）
- 收藏 3 词 → 菜单徽标 `3` → 浏览翻词/发音/自评正常 → SET 长按取消后词移出序列、状态栏计数同步收缩、清空后徽标显「空」
- 模式选择与长按下循环终态一致；配网/AP/LAN 从菜单进入与原直达行为等价
- 三色屏 env 菜单无局刷退化异常（partial_enabled=false 自动全刷）
- TINY/SMALL 几何派生正确（构建级验证，真机待 bring-up）

## 十一、风险与备选

| 风险 | 缓解 |
|---|---|
| 中键长按语义迁移肌肉记忆 | 开发期零存量用户；README/接线图同步防混淆 |
| CJK 反选白字渲染对比度（16px 点阵白字） | 与 wifi_ui 黑底白字 ASCII 同族；真机验收项 |
| 菜单与 wifi_config_ui 双激活时序 | activate 统一「先 exit 后 enter」纪律，menu_ui_is_active 在 exit 即清 |
| 收藏视图 SET 长按双重语义（收藏切换 vs 退出） | SET 长按=取消收藏（保留全局一致），退出走 RST 长按——键义总表明示 |
| 备选方案（未采纳）：双击键/组合键唤出菜单 | 不可发现 + button_handler 状态机扩展成本，仅作记录 |
