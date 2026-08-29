# AI 语音对话模式（P2）设计文档

> 状态：**P2A 后端 + P2B 固件全部交付（2026-08-24）**：单测 39/39 绿、固件
> 8 env 构建绿；实机验收：**ES8311+NS4150B 模块已集成（2026-08-29，
> 取代 INMP441，录音/播放一体）**，待实机 bring-up（与跟读评测共享，
> 见 `AI_SPEECH_ASSESSMENT.md` §4）。后端链路已全就绪：云 LLM 三云
> 可切（§8）+ 双语 ASR 达标（§7.3），仅余设备端上板。
> **A1-A3 学习扩展交付（2026-08-29）**：场景化对话/英→中翻译/中文
> TTS 路由/落库周报/生词收藏——§7 模式扩展协议；单测 110/110 绿、
> 8 env 构建绿。
> **三云端到端实测通过（2026-08-29）**：DeepSeek/Qwen/智谱三云全链路
> 验证（§8，含 emoji 剧显缺陷修复，单测 113/113 绿）；**双语 ASR
> 同日部署达标（§7.3：中文 20 句 CER 1.3%，中→英闭环实测通过）**。
> 关联：后端 `ChatService.cs` / `AsrService.cs` / `DeviceController.Chat`；
> 固件 `chat_mode.c` / `study_mode_machine.c`（MODE_CHAT）/ `main.cpp`
> （ui_render_chat / on_button 转发）。

## 1. 目标与定位

学生按中键与「英语外教」自由语音对话：设备录音上传，后端单端点完成
ASR → LLM → TTS，回传文本 + MP3 音频 URL：

- **AI 后端化红线**：ESP32-S3 只录音上传 / 下载播放，零设备端 AI。
- **语音优先、屏幕克制**：墨水屏不承担对话流渲染，反馈优先 haptic；
  局刷面板仅状态词 + 末句回复 ≤2 行，三色面板纯语音+震动。
- **儿童安全**：LLM 人设/黑名单/长度护栏均在后端，设备端零过滤逻辑。

## 2. 后端协议（已实现，冻结）

### 请求

```
POST /api/device/chat
X-Device-Key: {DeviceApiKey}
Content-Type: multipart/form-data; boundary=...

file= [wav 二进制文件字段（字段名固定 "file"，与 pronunciation 同款）]
```

- WAV 格式硬约束：**RIFF/PCM、16kHz、16bit、单声道、时长 ≤10s
  （约 320KB，硬上限 512KB）**——超限 413，格式/无声 400。
- 固件侧录音参数天然保证格式（`mic_recorder.c` max_ms=10000 档）。

### 响应

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "transcript": "what is your favorite animal",
    "reply": "I love cats! They are soft and funny.",
    "engine": "sherpa",
    "audioUrl": "/api/device/audio/chat_1724470400123.mp3"
  }
}
```

| 字段 | 说明 |
|:--|:--|
| `transcript` | ASR 转写（固件不消费，屏显克制） |
| `reply` | LLM 回复文本（已过黑名单/长度护栏） |
| `engine` | ASR 引擎标识（`sherpa`；`none` 时端点 503） |
| `audioUrl` | 回复 MP3 相对路径，GET 复用 P0B `/api/device/audio/{file}`（X-Device-Key）；**TTS 失败降级为 null**（仅文本回复） |

### 错误码（信封 code 与 HTTP 状态一致）

| HTTP | 场景 | 固件处置 |
|:--|:--|:--|
| 400 | 空 file / 非法 WAV / 无话音 | 长震 + 状态区提示，回 idle 不退模式 |
| 413 | >512KB | 同上（固件录音上限天然不触发） |
| 503 | ASR 不可用（Provider=none/缺模型） | 同上 |
| 502 | LLM 不可用 | 同上 |

## 3. 后端编排（ChatService 七步）

1. `ParsePcm`（与 pronunciation 共用 WAV 解析，对话档 512KB 上限）
2. ASR（`AsrService` / sherpa-onnx zipformer en；Provider≠sherpa 或缺
   模型恒 null → 503，不炸启动）
3. 会话上下文：Redis `chat:{deviceId:N}` 最近 24 条（12 轮），TTL 30 分钟
   （超时自然新会话）；**Redis 不可用降级无上下文单轮**（告警不阻断）
4. LLM（`IChatClient` 现有 Ai 节，Ollama qwen2.5:7b / 云可切）：
   system prompt = 英语外教 persona、A1-A2 词汇、儿童安全话题白名单、
   拒暴力恋爱话题、不说 AI 身份
5. 安全护栏：输出黑名单关键词命中 → 替换安全回复；`EnforceLimits`
   截断 ≤2 句 ≤40 词（TTS 时长约束）
6. TTS（P0B `TtsService.SaveClipAsync`）落 `chat_{ts}.mp3`；失败
   audioUrl=null 降级
7. 回写上下文（>24 条裁头）

清理：Hangfire `ChatAudioCleanupJob`（Cron.Hourly）回收 `chat_*` 超
1 小时文件；词条音频 `{guid}.mp3` 命名不冲突不误删。

### 部署（docker-compose）

```yaml
environment:
  Asr__Provider: "${ASR_PROVIDER:-none}"       # sherpa 启用
  Asr__ModelDir: "/data/sherpa/zipformer-en"
volumes:
  - sherpa:/data/sherpa    # 模型不入镜像层
```

模型来源：github.com/k2-fsa/sherpa-onnx releases
`sherpa-onnx-zipformer-en-2023-06-26`（encoder/decoder/joiner + tokens.txt，
~80MB）。与 M5 GOP 升级共用同一 NuGet 绑定（`org.k2fsa.sherpa.onnx`）
与挂载基建（`AI_SPEECH_ASSESSMENT.md` §5）。

## 4. 固件侧（chat_mode.c 五态状态机）

```
idle → recording(≤10s，VAD 断/中键说完即发) → uploading
     → thinking(下载回复 MP3) → playing → idle
                                    ↑ 网络失败：长震+「网络不可用」→ idle（不退模式）
```

- **MODE_CHAT 临时视图**（同 WRONGBOOK/COLLECTION 先例）：不入
  switch_next 轮换、不 NVS 恢复、快捷菜单「AI 对话」项进入；
  前置 Wi-Fi/设备 Key/SD 预检失败长震不进入。
- **常驻任务 + 轮询触发位**（6KB 栈，优先级 4）：单任务串行整轮，
  无并发 HTTP/播放竞态；退出置取消位，任务循环边界静默收尾
  （HTTP 阻塞期最长 45s 后自然退，渲染全跳过）。
- 录音复用 `mic_recorder_record(max_ms=10000)`（P1 参数化放宽档，
  320044B PSRAM 录完即释）；VAD 尾静音 800ms 自动断，中键说完即发
  （`send_now` 手动断保留已录）。
- 回复 MP3 写 `/sdcard/audio/chat_tmp.mp3` → `audio_play_file()`
  （零新播放路径）→ 播完即删；audioUrl null 时仅屏显文本。
- **按键**：中=idle 开始 / recording 说完即发 / playing 打断重说；
  长按中或 RST=退出回闪卡（`chat_mode_on_button` 返回 false 由
  main.on_button 编排退出）；其余键忽略。
- **haptic**：录音起一短震（KEYPRESS）、回复到两短震（PASS）、
  网络失败一长震（ERROR）、无声一短震轻提示。
- **屏显**（ui_render_chat）：进/出模式各一次全刷；环路内仅内容区
  局刷（状态词 AI Chat/Listening/Sending/Thinking/Speaking/Offline +
  末句回复 ≤2 行）；**三色面板（`epd_gfx_partial_supported()=false`）
  零渲染纯语音+震动**（与待机页轮换停用同款 UX 降级）。
- **电源零改动**：对话期每次按键 `power_note_activity()` 自然续期，
  退出 10 分钟后正常入睡；HTTP 单回合 <50s 不会跨越无操作窗口。

## 5. 验收清单（实机，ES8311 模块已集成，待 bring-up）

- [ ] 局域网 5 轮连续对话首响 ≤3s（录音 VAD 断 → 出声）
- [ ] 播放中按中键打断并直接重说（新录音不被旧回合干扰）
- [ ] 三色屏 env（inkword-s3-e042）纯震动模式可用（无屏显无卡死）
- [ ] 网络失败：长震 + 提示，回 idle 不退模式；恢复后可立即重试
- [ ] 深睡唤醒后对话正常（audio 懒初始化往返无残留）
- [x] 后端：curl 上传 WAV → 3-6s 返回 reply + audioUrl 可下载播放
      （P2A 单测 + Release 构建绿；全链路延迟实测待实机记录）

## 6. 二期增强

- 流式 chunked 回传（当前整句回传，首响受 TTS 全长制约）
- 云 ASR / 云 TTS（接口已留 `Asr:Provider` / `Tts:Provider` 切换位）
- ~~对话历史落库与学习分析~~ → 已交付（A3，2026-08-29，§7.4）
- ~~双语 ASR 基准验收~~ → 已达标（2026-08-29，§7.3：中文 20 句平均
  CER 1.3%，中→英闭环实测通过）

## 7. 模式扩展协议（A1-A3，2026-08-29 交付）

> 向后兼容冻结：query 参数可选，老固件不带 query = free，请求与响应
> 与 §2 现状逐字节一致；新响应字段全部可缺省（固件 cJSON 可选解析）。

### 7.1 请求扩展（A1 场景化对话 + 英→中翻译）

```
POST /api/device/chat?mode={free|scenario|translate}&scenarioId={code}
（multipart 字段 file 不变）
```

| 模式 | scenarioId | 行为 | 固件菜单入口 |
|:--|:--|:--|:--|
| free（缺省） | — | 英语外教自由对话（§2 现状） | 「自由对话」 |
| scenario | 必填（6 选 1） | 场景剧本对话（persona/sceneSetup/目标词注入） | 「场景对话」二级页 |
| translate | — | 翻译教练（A1 英→中；双语 ASR 后自动双向） | 「英中翻译」 |

- **场景库**：`ScenarioLibrary.cs` 静态内嵌 6 场景（food/directions/
  school/shopping/travel/doctor）；非法 mode/scenarioId 400
- **上下文键隔离**：free=`chat:{id:N}`（与现状一致零迁移）、scenario=
  `chat:{id:N}:s:{scenarioId}`、translate=`chat:{id:N}:t`；TTL 30min 不变
- **护栏分档**（ChatModeConfig）：free/scenario ≤2 句 ≤40 词；translate
  ≤4 句 ≤120 词（TranslateMaxBytes 480 字节 UTF-8 边界截断）
- **temperature**：translate 0.4（翻译准确性），其余缺省

### 7.2 响应扩展字段

| 字段 | 时机 | 说明 |
|:--|:--|:--|
| `mode` | 恒有 | 回显（free/scenario/translate） |
| `warmup` | 仅 scenario 首轮 | 场景中文预热 ≤180B（固定文案不走 LLM；首轮 speaking 态屏显） |
| `lang` | A2 起恒有 | "en"/"zh"：reply 主语言，TTS 音色路由依据 |
| `wordHits` | A3 起可空 | `[{text, cloudId}]` ≤5：transcript+reply 整词命中词库（大小写/标点归一，首现序封顶） |

### 7.3 中文语音（A2 全链路就绪：双语 ASR 已部署达标 2026-08-29）

- **TTS 双音色**：`Tts:PiperVoice`（en_US-lessac）+ `Tts:PiperVoiceZh`
  （zh_CN-huayan-medium，~63MB volume 挂载）；`SynthesizeMp3Async(lang)`
  参数化，默认 "en" 老调用点零改
- **语言路由**（ChatService 第 6 步）：free 模式回复含 CJK → zh 音色
  全文播报（学生可中文与外教对话）；translate 恒 en
  （FirstNonChineseLine 英文行播报 + 中文屏显）
- **translate 双向互译**：prompt 自动检测输入语言——说英语纠错译中、
  说中文译英，**双向均实测通过**（2026-08-29：中文输入 0.84s 返回
  英文句+中文两行，lang=en 路由正确）
- **双语 ASR 部署（已验收）**：sherpa-onnx-zipformer-zh-en-2023-11-22
  （312MB 包；实际加载 int8 encoder 66M + fp32 decoder 4.9M + int8
  joiner，`Asr__ModelDir=data/sherpa/zipformer-zh-en` + 三件套
  epoch-34-avg-19 文件名覆盖，与 zipformer-en 目录共存、环境变量即切
  零代码回滚）。**勘误**：本节原记名 zipformer-bilingual-zh-en
  （~200MB）有误，GitHub asr-models 无此包（404），实名如上
- **20 句基准（macOS Tingting 合成，chat 真实链路）**：平均 CER
  **1.3%**、19/20 句零错；最差两句为同音字替代（T恤→T去 10%、
  面条→火免条 15.4%，语义无损）。英文回归表观 WER 17.2%，主因
  分词伪错（TO DAY/TO MORROW 拆词，双语 BPE tokens 风格），归一
  后 <10%，真实回退仅 BEISHING 级个别词；单语 zipformer-en 目录
  保留，纯英文高要求场景可切回

### 7.4 学习联动（A3）

- **ChatTurn 落库**：八步编排末尾 Hangfire fire-and-forget（响应不
  等待，AutomaticRetry(0) 失败仅日志）；daily 04:00 清理 90 天前记录
- **wordHits**：CachedWordListProvider 5min 词库快照缓存（Singleton +
  IServiceScopeFactory 防 captive dependency），对话每轮不打卡
- **对话周报**：ChatReviewJob 周日 05:00 聚合近 7 天 → LLM 五段 JSON
  （summary/topics/highlights/suggestion/reviewWords）→ ChatReviews
  表 + Redis `chatreview:{id}` TTL 7 天；下发 `GET
  /api/device/chat-review`（Redis 热 → 表冷 → 404）；设备端：快捷
  菜单 [学习] 组「对话周报」项（MU_PAGE_REVIEW 分页只读页：加载/
  暂无/失败单帧 + OK 态两页（概况/建议+复习词），一次性任务拉取
  （按键零 HTTP），404 与网络失败屏显区分）
- **固件**：IDLE 态「本轮生词 N」计数行 + SET 短按收藏（触发位范式：
  按键零阻塞，任务上下文逐条推 /sync/collect 零新协议；三色屏仅震动）
- **菜单**：「AI 对话」→ 二级选择页（自由对话/英中翻译/场景对话 →
  6 场景滑动列表，MU_PAGE_CHATSEL/MU_PAGE_SCENARIO 复用模式选择页范式）

## 8. 三云端到端实测（2026-08-29，云 LLM 切换位验收）

### 环境

- 运行：brew PG@18/Redis + `dotnet run`（Docker.raw 损坏期间的本机替代）；
  密钥仅环境变量 `Ai__CloudApiKey`（不入仓库，与既有安全规范一致）
- ASR：sherpa-onnx-zipformer-en **int8 三件套**（66M encoder，
  `Asr__Encoder=*.int8.onnx` 覆盖，精度无损延迟更优）；本机 ASR+云 LLM
  端到端延迟见下表
- TTS：piper 缺席 → `audioUrl=null` 降级路径验证（链路不阻断，日志
  「piper 退出码 127」符合设计）

### 三云对比（同一 WAV 三模式实测，macOS say 合成语音）

| | DeepSeek deepseek-chat | Qwen qwen-plus | 智谱 glm-4-flash |
|---|---|---|---|
| free 端到端 | **3.0s** / 1.26s（上下文轮） | 2.74s | 4.80s |
| translate | 0.85s | 1.18s | 2.56s |
| scenario 人设 | 点餐剧本推进自然 | 良好 | **跳出人设**（医生场回助手腔） |
| 回复质量 | 追问推进对话最佳 | 良好（爱加 emoji，已后端剥离） | 偏短无追问 |
| 结论 | **主力** | 备选 | 免费兑底（儿童场景人设弱）

### 验证点清单（全部通过）

- 三模式：transcript/reply/engine/mode/lang/warmup/wordHits（5 命中
  含 cloudId）字段完整；free 上下文跨轮延续；scenario 首轮 warmup
  中文文案 + 人设保持；translate 两行格式（纠错+翻译）正确
- A3：ChatTurn 5 轮全部异步落库（Mode/ScenarioId 字段正确）；
  ChatReviewJob 手动触发（dashboard 批量 Trigger；curl 直发需
  `Content-Type: x-www-form-urlencoded`，否则 422/500）→ 五段
  JSON 高质量（topics 准确识别天气/点餐，reviewWords 从真实对话
  提取）；`GET /api/device/chat-review` DB 首读 85ms → Redis 热路径
  13ms（6.5x）

### 实测发现与修复

- **emoji 剧显缺陷（Qwen 实测发现）**：云 LLM 输出 emoji（🌞），墨水屏
  字体无字形渲染豆腐块 → `ChatService.StripEmoji`（护栏第 5 步入口，
  rune 过滤 U+1F000-1FAFF/2600-27BF/2B00-2BFF + VS16/ZWJ，空格收敛，
  CJK 原样保留）+ 3 条单测；修复后 Qwen 三轮复验干净。弯引号 U+201C/D
  在中文字体覆盖内，暂不处理（实测 Qwen 修复后输出正常）
