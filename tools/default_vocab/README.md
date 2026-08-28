# 系统默认词库（GitHub 开源中小学词库/古诗词）

InkWord 系统默认词库的**数据源下载、格式转换与产物说明**。默认组合已随
后端启动自动导入（`SeedData/default_words.csv`，Words 表为空时幂等执行，
见 `InkWord_Backend/src/InkWord.API/SeedDefaultWords.cs`）。

## 目录结构

```
tools/default_vocab/
├── .raw/                    # 原始数据（不入 git；download.py 重新下载）
│   └── download.py          # 数据源批量下载（走本机 7897 代理）
├── gen_default_vocab.py     # 转换脚本：原始数据 → 系统 CSV
├── fetch_audio.py           # 读音整理：真人音下载/转码/回填 audio 列（见下节）
└── out/
    ├── default_words.csv    # 默认组合 2407 条（后端 SeedData 的正本来源）
    ├── default_words.json   # 固件内嵌兜底词库（与 CSV 同源）
    ├── audio/               # 最终读音 32k mono MP3（不入 git，拷 SD 卡用）
    ├── audio_raw/           # API 原始下载留档（不入 git）
    ├── audio_api_report.json# 每词音源清单（api/youdao/none）
    └── subdicts/            # 教材分册词库 25 册（按需经管理端导入）
```

## 数据源与许可证

| 内容 | 来源 | 许可证 | 规模 |
|---|---|---|---|
| 中考核心词汇 | [qwerty-learner](https://github.com/RealKai42/qwerty-learner) `ZhongKaoHeXin.json` | GPL-3.0 | 2140 词 |
| 高考 3500 词 | 同上 `GaoKao_3500.json` | GPL-3.0 | 3877 词 |
| 人教版教材分册词汇（小学 8 册/初中 5 册/高中 11 册） | 同上 `PEP*.json` | GPL-3.0 | 10920 词 |
| 小学必背古诗词 | [AncientPoemsPrimary](https://github.com/MIZHANG08/AncientPoemsPrimary) | 无（注明来源） | 90 首 |
| 初中古诗文 | [Junior-Middle-School-poetry](https://github.com/tangyuan0821/Junior-Middle-School-poetry) | CC-BY-SA-4.0 | 117 篇 |
| 高考古诗文 60 篇 | [gaokao-poetry](https://github.com/clover-yan/gaokao-poetry) | CC-BY-SA-4.0 | 60 篇 |

> 词表/篇目本身属于事实性考纲数据；各仓库许可如上，再分发时保留本表出处。
> 备选全量库：[chinese-poetry](https://github.com/chinese-poetry/chinese-poetry)（MIT，34 万+ 篇）。

## 默认组合（2407 条）

| Tag | 条数 | Grade | 难度 |
|---|---|---|---|
| 中考核心 | 2140 | 初中 | 2 |
| 小学古诗 | 90 | 小学 | 1 |
| 初中古诗文 | 117 | 初中 | 2 |
| 高考古诗文 | 60 | 高中 | 3 |

## 硬约束（务必遵守）

- **设备端全库同步上限 4000 词**：`/api/device/sync/words` 按 Version 增量
  拉取全库（无 Tag 过滤），固件 `MAX_WORDS=4000`。**默认组合 2407 条已占用
  额度；导入任何分册前先核算总量**（如默认组合 + 人教版三上 64 条 = 2471 ✓，
  再叠加高考 3500 则 6284 ✗ 超限，设备学习状态将整体作废重建）。
- **CSV 为 Split(',') 简单格式**：后端 `AdminWordController.ImportCsv` 逐行
  `Split(',')`，**无引号转义**。生成端已把字段内英文逗号→中文逗号、引号→
  单引号、换行→空格。手工编辑 CSV 时同样不得引入英文逗号。
- **字段字节上限**（固件 `word_parser.h` 各宏 -4B 余量）：text 60 /
  phonetic 60 / meaning 252 / example 252 / tag 28 / source 60 / grade 20。
  语文长文（《阿房宫赋》等）超限自动截断加"……"。

## 字段映射

英语（qwerty-learner）：`name→Text`，`usphone(无则 ukphone)→Phonetic`，
`trans 用"；"连接→Meaning`，`Tag/Source/Grade 按词库元数据`。

语文（诗词）：`title→Text`，`author→Phonetic`（单词卡音标栏显示作者），
`content→Meaning`（超限截断），`首句→Example`。

## 读音文件整理（2026-08-27，英文 2140 条）

真人读音批量下载/统一规格/回填 audio 列，产物 `out/audio/{slug}.mp3`
（32k mono，与后端 TtsService ffmpeg 参数同规格）：

- **音源分层**：dictionaryapi.dev 词条 JSON 里 gstatic 域名真人音优先
  （api.dictionaryapi.dev/media 对本机网络挂起，已跳过）→ 有道 dictvoice
  美音兜底（几乎全覆盖含短语）；实测 2134/2134 唯一 slug 全量命中，
  Piper 兜底未启用。Google 系域名自动走 7897 代理（不可用回退直连）。
- **命名 slug**：小写 `[a-z0-9_-]`，空格→`_`，撇号/点删除（o'clock→oclock、
  p.m.→pm）；china/China、miss/Miss 等 6 组同音词对共用同一文件。
- **接入方式**：固件 `study_mode_machine` 人工命名字段优先（`/sdcard/audio/`
  下放同名文件即生效）；audio 列已回填 CSV+JSON 并同步双端副本
  （后端 SeedData/固件 src），中文古诗文 267 条留空待后续。

```bash
# 1. 下载（6 线程并发，全量约 8 分钟；幂等可断点续跑）
python3 fetch_audio.py api
# 2. 统一转 32k mono（需 brew install ffmpeg）
python3 fetch_audio.py norm
# 3. 回填 audio 列 + 同步后端 SeedData/固件 src（自动 .bakN 备份）
python3 fetch_audio.py fill
# 4. 拷贝到 SD 卡（自动探测 TF 卷；勿用裸 cp，会残留 ._ AppleDouble 垃圾）
python3 sync_tf_audio.py --sync
```

> 重跑 gen_default_vocab.py 会重写 CSV/JSON 丢失 audio 列，需再跑
> `fetch_audio.py fill` 重建（音频文件本身不受影响）。

## TF 卡校验与同步（2026-08-28）

`sync_tf_audio.py`：TF 卡（读卡器挂载 /Volumes/…）与本地部署源
对齐。词表基准 = out/default_words.csv audio 列；音频源 =
out/audio/。

- 默认只读校验：自动探测 TF 卷（优先含 audio/ 或 decks/ 的 FAT 卷），
  报告缺失/0 字节损坏/多余三维（同音共用组按词维度展开）；
- `--sync`：从 out/audio/ 补齐缺失（0 字节重拷；tmp+rename 防半文件）；
- `--sync --prune-extra`：同时清理词表已无对应词条的多余文件。

**同卡同源注意**：TF 卡根 words.json 必须与固件内嵌同版本同步
（固件装载三级递降：活跃卡组 → SD words.json → 内嵌兜底，SD 旧词库
会遮蔽内嵌新版，见设计文档 CATALOG_BROWSE_VOICE_SEARCH §10.5）：

```bash
# 词表变更后同步 TF 卡词库（勿用 Finder/cp，避免 ._ 垃圾；拷后可 find 清理）
cp ../../InkWord_Firmware/src/default_words.json /Volumes/TF卡/words.json
find /Volumes/TF卡 -name "._*" -delete   # 清 AppleDouble 残留
python3 sync_tf_audio.py                # 音频复核
diskutil eject /Volumes/TF卡            # 安全弹出
```

## 使用方法

```bash
# 1. 下载原始数据（需本机 7897 代理；改 PROXY 常量可换）
cd tools/default_vocab/.raw && python3 download.py

# 2. 生成 CSV（自动校验：列数/字节上限/无英文逗号撕裂/4000 总量）
cd tools/default_vocab && python3 gen_default_vocab.py

# 3. 同步默认词库到后端 SeedData（后端启动 seed 读取该副本）
cp out/default_words.csv ../../InkWord_Backend/src/InkWord.API/SeedData/

# 3b. 同步内嵌兜底词库到固件（embed 进固件 rodata，无 SD 卡开箱即用；
#     固件重烧后生效。与 CSV 同源同序生成，id/difficulty 数字契约对齐
#     后端 export；无 cloudId = 本地词条，评分/收藏不上报）
cp out/default_words.json ../../InkWord_Firmware/src/

# 3c. 同步 TF 卡（插读卡器；words.json 遮蔽内嵌，见「TF 卡校验与同步」节；
#     音频用 sync_tf_audio.py --sync 补齐，勿裸 cp 以免 ._ 垃圾）
cp out/default_words.json /Volumes/TF卡/words.json && python3 sync_tf_audio.py --sync

# 4. 分册按需导入：管理端「词库管理 → 导入 CSV」上传 out/subdicts/*.csv
#    （导入前核算全库总量 ≤ 4000，见上节硬约束）

# 5. 固件侧端到端验证（可选，双端对拍纪律）：后端导出 words.json
#    后喂给固件原生解析器，断言 2407 条×11 字段逐字节一致
#    cd InkWord_Backend && dotnet run   # 另一终端登录后：
#    curl -H "Authorization: Bearer $TOKEN" localhost:5228/api/admin/words/export -o /tmp/words.json
#    cd InkWord_Firmware && WORDS_JSON=/tmp/words.json /opt/homebrew/bin/pio test -e native-test
#    （缺 WORDS_JSON 时该用例 IGNORE；注意 PATH 里的 ~/Library/Python/3.9/bin/pio
#     是旧版，须用 penv 的 /opt/homebrew/bin/pio）
```

> **重跑警示**：`gen_default_vocab.py` 从 .raw 重生成时，单元细分
> （source/grade 列回平铺「中考考纲核心词汇/初中」）与 audio 列均会
> 丢失；需重跑归类管线 `InkWord_Firmware/tools/textbook_units.py`（v3，
> 需 QW_KEY/DS_KEY 环境变量）+ `textbook_units_apply.py` 落地，再
> `fetch_audio.py fill` 回填 audio（见设计文档 §10 数据链）。
