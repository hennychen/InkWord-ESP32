# AI 语音对话模式（P2）设计文档

> 状态：**P2A 后端 + P2B 固件全部交付（2026-08-24）**：单测 39/39 绿、固件
> 8 env 构建绿；实机验收待 INMP441 接线（与跟读评测共享，见
> `AI_SPEECH_ASSESSMENT.md` §4）。
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

## 5. 验收清单（实机，待 INMP441）

- [ ] 局域网 5 轮连续对话首响 ≤3s（录音 VAD 断 → 出声）
- [ ] 播放中按中键打断并直接重说（新录音不被旧回合干扰）
- [ ] 三色屏 env（inkword-s3-e042）纯震动模式可用（无屏显无卡死）
- [ ] 网络失败：长震 + 提示，回 idle 不退模式；恢复后可立即重试
- [ ] 深睡唤醒后对话正常（audio 懒初始化往返无残留）
- [x] 后端：curl 上传 WAV → 3-6s 返回 reply + audioUrl 可下载播放
      （P2A 单测 + Release 构建绿；全链路延迟实测待实机记录）

## 6. 二期增强（不在本期）

- 流式 chunked 回传（当前整句回传，首响受 TTS 全长制约）
- 云 ASR / 云 TTS（接口已留 `Asr:Provider` / `Tts:Provider` 切换位）
- 对话历史落库与学习分析（当前仅 Redis 会话窗口）
