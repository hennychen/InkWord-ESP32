# 教材目录浏览 + AI 语音查词 设计文档

> 状态：**后端 + 固件全部交付（2026-08-28）**：dotnet test 90/90 绿（含
> VoiceSearchTests）、native-test 40 过 0 失败（catalog_index 9 用例）、
> 8 env 构建绿（涉改文件零新增警告）；实机验收待排期。
> **单元细分增补（同日）**：2140 考纲词已按人教五册 58 单元重分类
> （§10 数据链），目录三级结构全量落地；TF 卡 words.json/audio 同步
> 核对完成（§10.5，SD 旧词库遮蔽内嵌问题已修）。
> 关联：后端 `VoiceSearchService.cs` / `DeviceController.VoiceSearch`；
> 固件 `catalog_index.c` / `browse_mode.c` / `voice_search.c` /
> `study_mode_machine.c`（MODE_BROWSE/MODE_VOICE + seek）/ `menu_ui.c` /
> `main.cpp`（装载挂接 + 按键/渲染分流）；数据管线
> `InkWord_Firmware/tools/textbook_units.py`（v3）+ `textbook_units_apply.py`。

## 1. 目标与定位

解决「4000 词大词库里快速找到要学的词」：不改变闪卡学习主线，提供
两个**定位入口**，定位即学习起点（游标跳到该词闪卡，后续翻词按词库
顺序继续）。

| 决策点 | 结论（用户裁定 2026-08-28） |
|:---|:---|
| 定位行为 | 跳到该词的闪卡继续翻词，不另建学习会话 |
| ASR 语言 | 一期英语（sherpa zipformer-en 现有模型）；中文古诗文二期挂模型 |
| 目录粒度 | 年级 + 单元两级（source 已含「人教版 九年级 Unit 5」结构） |
| 设计范围 | 设备端固件 + 后端；App/Admin 列为后续增强 |

红线遵守：**WordEntry 禁止新增字段**（目录索引只消费现有
grade/source）；**AI 全后端化**（设备只录音上传 + 消费候选，
零设备端 AI）。

## 2. 功能 A：教材目录浏览（MODE_BROWSE）

### 2.1 目录索引 `catalog_index.c`（纯 C，native-test 可测）

- **数据源**：word_parser 词池的 `grade`（≤24B）/`source`（≤64B）字段。
- **构建算法**（计数 → 排序 → 前缀和 → 游标填充，四遍稳定）：
  1. Pass 1 收集 distinct 年级（容量满末槽改造「(其他)」兜底）；
  2. Pass 2 单遍计数：`locate_grade` 定位年级桶，单元名 distinct 暂存
     + 计数（容量满并入「(未分类)」桶）；
  3. Pass 3 每年级栈上排序（数字 Unit 优先、字典序跟随）+ 前缀和算
     各桶 `start`，初始化填充游标；
  4. Pass 3b 按词库序游标填充 `s_entries`，单元内天然稳定。
  > 直译教训：桶间交错写入时「首词位 + 词数」描述区间不成立
  > （native 测试捕获），计数-前缀和方案根治。
- **单元名解析**：source 末尾匹配最后一个 `unit`（不区分大小写）截到
  结尾，回看保留 `Starter` 前缀，话题名保留到串尾（"人教版 九年级
  Unit 5 What are the shirts made of?" → "Unit 5 What…"，前置版本/
  年级由 grade 层承载）；无该模式以 source 整串去重；source 空归
  「(未分类)」。年级空归「(未分类)」；词库 grade 全空时单桶「全部
  词汇」直入词表。
- **年级排序**：预定义序表模糊包含（小学→七上→七下→八上→八下→
  九→考纲拓展→中考→高考→初中→高中，2026-08-28 单元细分增补分册
  与拓展/小学桶），未匹配字典序尾置「(其他)」。
- **内存**：词序号数组 uint16_t ×4000 + 桶头 ≈10KB PSRAM
  （`heap_caps_malloc`，独立于词池分配）；词库切换 `free` 重建。
- **挂载点**：`main.cpp` `load_words_with_catalog()` 包装
  `load_active_words + catalog_build`，setup 装载 / `deck_flow_switch`
  两条链路（含回退分支）/ INKWORD_DEMO_WORDS 演示词分支统一走尾部
  构建口径。
- **容量**：CATALOG_GRADE_MAX=16 / CATALOG_UNIT_MAX=16 /
  CATALOG_NAME_MAX=48（单元名含话题名如 "Unit 3 Could you please…"，
  UTF-8 名截断回退 continuation 字节安全；32→48 随单元细分增补）。
- **单元排序**：`[Starter] Unit N`（含话题名后缀）按数值序，Starter
  键 N 排正课（N+1000）前且各自升序；非 Unit 名按字典序。

### 2.2 三级视图 `browse_mode.c`

```
CATALOG_GRADE（年级列表）→ CATALOG_UNIT（单元列表）→ CATALOG_WORDS（词表滚动）
```

- 词表行 = text + meaning 首行（`\n` 截断）单行混排截断；反选高亮 +
  滚动条 + 页码（4000 词翻页不迷路，按住连发按键框架已支持）。
- 几何经 BR-\* layout_profile 派生宏（MU-\* 同款），TINY/SMALL/MID
  全档零特判；列表绘制在本模块内实现（复用 epd_gfx/cjk_text，
  不抽公共组件——最小变更）。
- **按键**：上/下移动；中=进入下一级 / 词表页定位该词；RST 短按=
  逐级返回（词表→单元→年级→退出）；RST 长按=直退回闪卡；SET 短按
  映射 RST 短按。
- **跳转链路**：中键选词 → `study_mode_seek(word_index)` → 退出视图
  进 FLASH 词卡。
- **刷新**：进视图/换级全刷；词表/目录光标移动清列表区单遍局刷，
  阈值 `refresh_gfx_before_partial_n(10)` 升级保养；三色面板自动
  全刷（无局刷能力位，低频翻页可接受）。

### 2.3 `study_mode_seek(int word_index)`

切 FLASH 模式 + 游标=index（钳位 [0, total-1]）+ 渲染词卡；不写 NVS
（FLASH 本就可恢复）。browse 选词与 voice 候选确认共用。

## 3. 后端协议（已实现，冻结）

### 请求

```
POST /api/device/voice-search?deck={Deck.Code}
X-Device-Key: {DeviceApiKey}
Content-Type: multipart/form-data; boundary=...

file= [wav 二进制（字段名 "file"，pronunciation/chat 同款）]
```

- WAV 硬约束：RIFF/PCM、16kHz、16bit、单声道、≤5s；上限 256KB
  （超限 413，固件录音 3s 上限天然不触发）。
- `?deck=` 限定设备活跃词书范围；失配/缺省兜底全库（归档词恒排除）。

### 响应

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "transcript": "ability",
    "candidates": [
      { "text": "ability", "meaning": "n. 能力；才能",
        "cloudId": "…guid…", "score": 1.0 }
    ]
  }
}
```

| 字段 | 说明 |
|:--|:--|
| `transcript` | ASR 转写（固件屏显「」引号行） |
| `candidates` | top-5；空列表=无命中（固件提示重说） |
| `text/meaning/cloudId/score` | 词文本 / 释义截 60 字符 / Word.Id / 层级分 |

### 错误码（信封 code 与 HTTP 一致，chat 同款）

| HTTP | 场景 | 固件处置 |
|:--|:--|:--|
| 400 | 空 file / 非法 WAV / ASR 空文本（无声） | 短/长震 + 提示回 idle 不退模式 |
| 413 | >256KB | 长震（固件录音上限天然不触发） |
| 503 | ASR 引擎不可用（Provider=none/缺模型） | 长震 + 提示 |

## 4. 后端编排（VoiceSearchService 四步）

1. `ParsePcm`（复用 PronunciationService 的 WAV 解析，查词档 256KB）；
2. ASR（`IAsrTranscriber` / sherpa-onnx zipformer-en 懒加载，
   不可用 503 不炸启动）；
3. 范围查询：`?deck` Code→DeckId→该组词（AsNoTracking），失配兜底
   全库；
4. 文本归一（`Normalize`：小写、剔除非字母数字、压空白）→ 三级匹配
   → top-5（同分按文本字典序，结果稳定可复现）。

| 层级 | 规则 | 分 |
|:--|:--|:--|
| L1 | 精确全等 | 1.0 |
| L2 | 前缀/包含（双向，短边 ≥3 防冠词误命中） | 0.7 |
| L3 | Levenshtein ≤2 且词长 ≥5 且长度差 ≤2（兜 ASR 拼写漂移） | 0.4 |

同词多路径命中取最高分；`Normalize/ScoreWord/EditDistance` 纯函数
公开供单测。

## 5. 固件侧（voice_search.c 四态状态机）

```
idle(中键录音) → recording(≤3s，VAD 断/中键提前停) → searching(上传)
     ↑ result(候选列表，上下移动/中确认/RST 重说) ←——┘
     网络失败：长震 + 提示 → idle（不退模式）
```

- **MODE_VOICE 临时视图**（第五先例）：不入 switch_next 轮换、不 NVS
  恢复；前置 Wi-Fi + 设备 Key（**无 SD 依赖**，PSRAM 缓冲直传）。
- **常驻任务 + 轮询触发位**（6KB 栈 / 优先级 4，chat 同款纪律）：
  按键上下文只置 `s_round_req/s_cancel/s_send_now` 标志；退出置
  取消位，任务循环边界静默收尾（HTTP 阻塞期 ≤20s 后自然退）。
- 录音复用 `mic_recorder_record(max_ms=3000)`；上传 multipart 三段
  流式写（chat_upload 同构）+ `X-Device-Key` + `?deck=` 活跃卡组。
- **跳转**：候选 cloudId 在当前词库线性匹配 index → `study_mode_seek`；
  未命中（词不在当前词书）提示「不在当前词书」回候选列表。
- **haptic**：录音起一短震（KEYPRESS）、候选到达两短震（PASS）、
  网络/引擎失败一长震（ERROR）。
- **三色屏降级**（面板能力位先例）：无局刷面板本模块**零渲染**，
  ASR top-1 直接震动 + 跳转（想换词 RST 重说）；局刷面板走标准
  5 候选列表（transcript 行 + 反选 + 提示栏）。
- 渲染刷新：状态迁移全刷；result 候选移动清列表区局刷（阈值 10
  升级保养）。

## 6. 菜单集成（menu_ui [学习] 组）

```
[ 学习 ]  收藏列表 / 教材目录 / 语音查词 / 模式选择 / AI 对话
```

（主列表 13→15 行；两项按使用频率前插收藏之后）

- **教材目录**：act_browse → `study_mode_enter_browse()`（前置词库
  ≥1，不满足长震回学习页）+ `browse_mode_reset()` 清态首帧。
- **语音查词**：act_voice_search → `study_mode_enter_voice_search()`
  （前置 Wi-Fi/Key，chat 预检先例）+ `voice_search_reset()` 清态起任务。

## 7. 测试与验收

| 层 | 内容 | 结果 |
|:--|:--|:--|
| 后端单测 | L1/L2/L3 分层命中、去重取高分、deck 过滤与兜底、空文本/超限 400/413、无鉴权 401 | 90/90 绿 |
| 固件 native | grade 桶化 / Unit 解析（Starter 回看+话题序）/ 序表扩展 / 空值归桶 / 交错写入稳定性 / UTF-8 截断 | 40 过 0 失败 |
| 构建矩阵 | 8 env（含三色 ×3、BW 骨架 ×2、demo） | 全 SUCCESS |

实机验收清单（待排期）：

- [ ] 三级目录浏览 → 跳词卡后翻词连贯（定位即学习起点）
- [ ] 语音查典型词（apple / ability / 近音词）top-5 命中
- [ ] 三色屏（inkword-s3-e042 等）top-1 直达不卡死
- [ ] 深睡唤醒后两功能正常（索引随装载链路重建）

## 8. 风险与缓解

- **ASR 对单词短促发音识别弱** → VAD 断点 + 3s 档 + L3 编辑距离
  兜底；实测命中率记入本文档（验收后回填）。
- **source 自由文本导致单元桶碎** → 宽松去重 +「(未分类)」兜底；
  Admin 端 source 规范化列二期。
- **4000 词滚动疲劳** → 页码 + 滚动条 + 按住连发。
- **PSRAM 常驻 +10KB** → 词池 7.9/8MB 预算内（索引独立分配）。

## 9. 二期增强（不在本期）

- 中文古诗文 ASR（sherpa 中文模型挂载，协议零改动）
- Admin 端 source/grade 规范化工具（目录桶质量提升）
- App 端目录浏览 / 语音查词镜像入口

## 10. 单元细分数据链（2026-08-28 增补）

### 10.1 问题与数据形态

上线验收发现目录「不够细分」：内嵌词库 2140 中考词全部
source=「中考考纲核心词汇」/grade=「初中」平铺，目录年级层只有一桶、
单元层空转。数据形态变更为（零 WordEntry 增量，只改两列值）：

| 字段 | 旧 | 新 |
|:--|:--|:--|
| grade | 初中（平铺） | 七年级上/七年级下/八年级上/八年级下/九年级/考纲拓展 |
| source | 中考考纲核心词汇 | `Unit N 话题名`（如 "Unit 5 What are the shirts made of?"）/ 考纲拓展词汇 |

语文行（小学古诗/初中古诗文/高考古诗文 267 行）不动；小学已学英语
词（LLM 判定 75 个）归七上 Starter 1-3 复习单元轮转（Starter 本就是
小初衔接复习单元，apple/pen 在此复现符合教材语义）。

### 10.2 归类管线（`InkWord_Firmware/tools/textbook_units.py` v3）

单元清单硬编码（人教 Go for it! 2012 五册 58 单元，话题名人工核准；
密钥经环境变量 QW_KEY/DS_KEY 传入不落盘）：

1. **双模型交集**：qwen-flash 与 deepseek-chat 各 2 次采样生成「单
   元新学生词表」（严格 prompt），单模型自并集后跨模型交集；
2. **首现原则**：词 → 最早 (册序, 单元序)（学习顺序）；
3. **统一标注通道**：未覆盖词批量标注，prompt 附全部 58 单元话题
   清单锚定 + 「小学已学」出口 + 「不在教材」出口，30 词/批 × 4 线
   程并行，册/单元号 BOOKS 内校验；
4. **兜底**：仍未匹配 → 考纲拓展桶。

**迭代教训**（v1→v3）：双模型并集词表虚胖致首现错吸（七上吸入 68%
考纲词）；裸标单元号存在前倾偏差（LLM 对不确定词倾向标早，七上
Unit 1 虚至 173 词）；v3 话题清单锚定 + 交集后分布收敛至教材真实形
态（七上 520 含 Starter 75 复现 / 七下 389 / 八上 507 / 八下 265 /
九 445 / 拓展 14）。

**质量抽查**：命中 2126/2140（99.3%，交集 838 + 标注 1288）；关键
词定位抽检 13 个全对（opera→九 U9 音乐话题、refrigerator→八上 U8
奶昔制作、disabled→八下 U2 志愿服务等）；拓展桶 14 词留档
`InkWord_Firmware/tools/units_report_20260828.md` 供人工复核。

### 10.3 落地产物（`tools/textbook_units_apply.py`）

/tmp 临时 CSV → 全量校验（2408 行/11 列/除 source-grade 外与原表全
同/字节上限/无英文逗号引号）→ 同步四产物：

- 后端种子 `InkWord_Backend/src/InkWord.API/SeedData/default_words.csv`
- 固件内嵌 `InkWord_Firmware/src/default_words.json`（write_json 契约：
  id 递增/difficulty 数字/空字段省略/version=条数/compact）
- `tools/default_vocab/out/default_words.{csv,json}`（生成产物目录同步）

**安全收紧**：九 U7 话题缩至 54B（原 63B 超 CSV 60B 安全线，固件
WORD_SOURCE_MAX=64 无余量）；八上 U10 话题去英文逗号（后端
`Split(',')` 无转义会撕裂列）。固件 Flash 占用 80.9%→82.1%
（+37KB，单元名变长），余量充足。

### 10.4 后端同步影响

`SeedDefaultWords` 读 CSV 种子，grade/source 变更无 schema 影响；
设备端已导入旧词库的用户不受影响（目录索引对任意 source/grade 组
合鲁棒，旧数据走「(其他)/(未分类)」兼容路径），管理端重导或重新
同步后生效新目录。

### 10.5 TF 卡同步与音频核对（2026-08-28）

**背景**：固件词库装载三级递降（活跃卡组 → SD `words.json` → 内嵌
兜底），插卡时 **SD 旧词库会遮蔽固件内嵌新版**——单元细分落地后
必须同步 TF 卡，否则设备上目录仍读旧平铺数据。

核对与修复（TF 卡经读卡器挂载 /Volumes/…，工具
`tools/default_vocab/sync_tf_audio.py`）：

| 项 | 结果 |
|:--|:--|
| 音频匹配 | TF 卡 audio/ 2134 文件与词表需求完全一致（缺失/损坏 0；单元细分不动 text/audio 列，需求集合未变；2140 词→2134 文件含 6 组同音共用） |
| words.json | 旧版平铺 → 已覆盖为新版单元细分数据（旧版备份 `out/tf_backup_20260828/`） |
| AppleDouble 垃圾 | 2136 个 `._*` 元数据文件（历次 Finder/cp 拷贝产生，魔数 00051607）已清理 |

工具用法：默认只读校验（自动探测 TF 卷，缺失/损坏/多余三维报告）；
`--sync` 从 `out/audio/` 补齐缺失（0 字节视为损坏重拷）；
`--sync --prune-extra` 同时清理词表已无对应词条的多余文件。后续
词表/音频变更后插卡一跑即可核对，替代裸 `cp`（后者会再生成 `._`
垃圾并遮蔽问题不可见）。
