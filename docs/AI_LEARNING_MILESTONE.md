# AI 学习功能 A1-A3 阶段交付总结（2026-08-29）

> 状态：**软件侧 100% 交付，实机 bring-up 待启动**。AI 学习功能全景
> 路线图 A1-A3 三项扩展 + 三云端到端验收 + 双语 ASR 部署达标 +
> ES8311+NS4150B 音频模块集成收口，全部于 2026-08-29 同日完成。
> 单测 113/113 绿、固件 8 env 构建绿。技术细节分存于
> [AI_CHAT_MODE.md](AI_CHAT_MODE.md) §7/§7.3/§8、[ROADMAP.md](ROADMAP.md)、
> [WIRING_DIAGRAM.md](WIRING_DIAGRAM.md) §2.7，本文为索引与结论。

## 1. 交付总览

| 项 | 内容 | 状态 |
|:--|:--|:--|
| A1 场景化对话 | 6 场景剧本库 + scenario 模式协议 | ✅ |
| A2 英中翻译 | translate 双向互译 + 中文 TTS 双音色路由 | ✅ |
| A2 双语 ASR | zipformer-zh-en 部署 + 20 句基准 + 中→英闭环 | ✅（2026-08-29） |
| A3 落库与周报 | ChatTurn 落库 + 对话周报 Job/端点/设备页 + 生词收藏 | ✅ |
| 三云 E2E | DeepSeek/Qwen/智谱全链路实测 + emoji 缺陷修复 | ✅（§8） |
| ES8311 收口 | 录音/播放一体模块集成（取代 INMP441），固件注释与文档同步 | ✅（待实机） |

## 2. A1 场景化对话

- **剧本源**：`ScenarioLibrary.cs` 静态内嵌 6 场景（点餐/就医等），
  C# 数组字面量而非 JSON 资源——编译期检查、零解析零加载失败路径；
  Id 用 ASCII 短码（URL query 与固件 `scenario[16]` 缓冲友好）
- **首轮体验**：中文预热 `warmup` ≤180B 固定文案不走 LLM（speaking 态
  屏显）+ 英文开场白 `OpeningLine` 注入 assistant（不走 LLM）
- **人设**：Persona/SceneSetup/TargetWords(≤5) 拼入 system prompt，
  护栏分档 free/scenario ≤2 句 ≤40 词
- **设备端**：「AI 对话」二级选择页（MU_PAGE_CHATSEL →
  MU_PAGE_SCENARIO 6 场景滑动列表，复用模式选择页范式）

## 3. A2 英中翻译 + 中文语音

### 3.1 双语 ASR 部署（已验收）

- **模型勘误**：原设计文档记名 `sherpa-onnx-zipformer-bilingual-zh-en`
  有误（GitHub asr-models 404）；实名 **sherpa-onnx-zipformer-zh-en-
  2023-11-22**（312MB 包，非流式 Transducer，与 OfflineRecognizer 零
  代码兼容；流式版 streaming- 前缀与 Paraformer 双语均不适用）
- **部署组合**：int8 encoder(66M) + fp32 decoder(4.9M，包内无 int8)
  + int8 joiner(1.0M)，`data/sherpa/zipformer-zh-en/`，环境变量
  `Asr__ModelDir` + 三件套 epoch-34-avg-19 文件名覆盖；与 zipformer-en
  目录（342M）共存，指回 en 目录即零代码回滚
- **20 句基准**（macOS Tingting 合成，chat 真实链路）：平均 CER
  **1.3%**、19/20 零错；最差 15.4%（面条→火免条，同音字语义无损）。
  英文回归表观 WER 17.2%，主因双语 BPE 分词伪错（TO DAY/TO MORROW
  拆词），归一后 <10%——非真实识别回退
- **中→英闭环实测**：translate 模式中文输入 0.84s 返回英文句 +
  中文两行（lang=en 路由正确），A2「说中文→译英文」设计首次
  全链路验证通过（设备实机待 bring-up）

### 3.2 中文 TTS 双音色路由

- `Tts:PiperVoiceZh`（zh_CN-huayan-medium，~63MB）+ 原英文音色；
  `SynthesizeMp3Async(lang)` 参数化，老调用点零改
- 路由：free 模式回复含 CJK → zh 音色全文播报；translate 恒 en
  （FirstNonChineseLine 英文行播报 + 中文屏显）

## 4. A3 对话落库与周报

- **ChatTurn 落库**：八步编排末尾 Hangfire fire-and-forget（响应不
  等待，AutomaticRetry(0) 失败仅日志——对话日志丢失可接受、重试
  风暴不可接受）；append-only 不继承 BaseEntity、纯 Id 关联无 FK；
  daily 04:00 清理 90 天前记录
- **对话周报**：ChatReviewJob 周日 05:00 聚合近 7 天 → LLM 五段 JSON
  （summary/topics/highlights/suggestion/reviewWords）→ ChatReviews
  表 + Redis `chatreview:{id}` TTL 7 天；下发 `GET /api/device/
  chat-review`（Redis 热 13ms → 表冷 85ms → 404）；设备快捷菜单
  [学习]「对话周报」MU_PAGE_REVIEW 分页只读页（按键零 HTTP）
- **生词收藏**：wordHits（transcript+reply 整词命中词库 ≤5 含
  cloudId，CachedWordListProvider 5min 快照缓存）+ 固件 IDLE 态
  「本轮生词 N」计数 + SET 短按收藏（触发位范式，任务上下文逐条推
  /sync/collect 零新协议）

## 5. 三云 E2E 实测与切换位验收（2026-08-29）

| | DeepSeek deepseek-chat | Qwen qwen-plus | 智谱 glm-4-flash |
|---|---|---|---|
| free 端到端 | **3.0s** / 1.26s（上下文轮） | 2.74s | 4.80s |
| translate | **0.85s** | 1.18s | 2.56s |
| 场景人设 | 剧本推进自然 | 良好 | 跳出人设 |
| 结论 | **主力** | 备选 | 免费兑底 |

- 密钥仅环境变量 `Ai__CloudApiKey`（不入仓库）；切换零代码改 env
- **emoji 剧显缺陷（Qwen 实测发现）**：云 LLM 输出 emoji 墨水屏无
  字形渲染豆腐块 → `ChatService.StripEmoji`（rune 过滤 U+1F000-
  1FAFF/2600-27BF/2B00-2BFF + VS16/ZWJ，CJK 原样保留）+ 单测，
  修复后三轮复验干净

## 6. ES8311+NS4150B 音频模块集成收口（取代 INMP441）

- **架构**：CODEC 模块一体承担录音（板载/FPC 模拟麦 + ADC）与播放
  （NS4150B 功放）；I2C 0x18（GPIO38/39），I2S 全双工共享时钟
  BCLK/WS = GPIO4/5、DAC_DAT=6、ADC_DAT=11，无 MCLK；录音期向 TX
  写静音零样本防功放杂音
- **固件收口**：音频子系统此前已完成迁移（es8311.c/h、
  mic_recorder.c 读 ES8311 ADC），本日仅 `mic_recorder.h` 文件头
  INMP441 残留注释修正（注释准确性红线）
- **文档同步**：WIRING §2.7 / PRD L84+L90 / AI_CHAT_MODE 头部+§5 /
  ROADMAP 支线行，统一为「模块已集成（2026-08-29），待实机验收」

## 7. 测试与质量基线

- 后端单测 **113/113 绿**（含 emoji 剥离、wordHits、翻译两行格式等）
- 固件 **8 env 构建绿**
- 基准资产（本机 /tmp，不入库）：`/tmp/asr_bench.py`（CER/WER 评测，
  走 chat 真实链路）、`/tmp/bench_gen.sh` + `/tmp/zh_*.wav`（20 句
  语料）、`/tmp/inkword_env.sh`（双语 + DeepSeek 环境变量）

## 8. 遗留与下一步

**唯一遗留：设备端实机 bring-up**（软件全就绪）：

1. 确认设备屏型 → 选对应 platformio env（烧错 env 会花屏）
2. 构建烧录（串口 /dev/cu.usbserial-0001，CP2102）
3. 后端 0.0.0.0:5228 监听 + 设备 NVS 写 `api_url=http://192.168.10.185:5228`
   （经配网 App/BLE 写入，固件无 console 命令）
4. **VAD 阈值复标**（mic_recorder.c `TODO: VAD_CALIB`）：-35dBFS 原为
   INMP441 标定，ES8311 模拟麦+PGA(gain=3, 18dB) 噪声底不同，建议
   -30~-40 dBFS 范围扫描
5. 串口日志排查（pio monitor 曾 traceback，需换串口读法或查占用）

**A4 思路池**（按价值排期）：听力小课 / 例句跟读纠错 / AI 测验增强 /
作文批改 / 家长周报 / 剧本 UGC。
