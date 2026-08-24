# AI 语音跟读评测（路径 C）设计文档

> 状态：**M5.1/M5.2 全部交付（2026-08-24）**：后端在线、固件随 P1 落地（8 env 构建绿），
> 待 ES8311 实机验收（§4 末项）。P2B 对话模式复用本模块录音本体（10s 放宽档）。
> 关联：PRD V2.1 P4「语音跟读评分」；后端 `PronunciationService.cs` / `DeviceController.Pronunciation`；
> 固件 `mic_recorder.c` / `study_mode_machine.c`（pron_task 听-跟一体流）。

## 1. 目标与定位

学生在墨水屏词卡上跟读单词，设备录音直传后端评分，按分数震动反馈：

- **AI 后端化红线**：ESP32-S3 只采集/上传/消费评分，绝不在设备端跑声学模型。
- 评分写回 `LearningRecords.LastPronScore`，为后续学习分析积累发音数据。
- 当前后端为**信号质量启发式引擎**（`engine: "heuristic"`），仅区分「读满/没读/
  环境噪声」，无音素级辨析；sherpa-onnx GOP 升级路径见 §5，协议不变。

## 2. 后端协议（已实现，冻结）

### 请求

```
POST /api/device/pronunciation?wordId={Guid}
X-Device-Key: {DeviceApiKey}
Content-Type: multipart/form-data; boundary=...

file= [wav 二进制文件字段（字段名固定 "file"，与 /sync/progress 鉴权同为
      X-Device-Key 头，非 Bearer）]
```

- WAV 格式硬约束：**RIFF/PCM、16kHz、16bit、单声道、时长 ≤3s（约 96KB，硬上限 128KB）**
  ——格式不符返回 400，消息体指明原因。
- wordId 为云端词条 Guid（同 `/sync/progress` 的 `ProgressItem.WordId`，即
  words.json 的 `cloudId`，固件侧对应 `WordEntry.cloud_id`）。

### 响应

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "total": 82,
    "durationMs": 640,
    "engine": "heuristic",
    "phonemes": [ { "phoneme": "S", "score": 90, "startSec": 0.10, "endSec": 0.22 } ]
  }
}
```

- `total`：0~100 总分；**≥60 视为通过**（与 PRD 定义一致）。
- `phonemes`：音素级明细，仅 GOP 引擎提供；启发式引擎恒为空数组，固件据此降级提示。
- `engine`：评分引擎标识（`heuristic` / `gop`），固件/前端可展示「基础评分/精细评分」。
- 评分成功即写 `LastPronScore`（无学习记录时顺带建档，SM-2/FSRS 状态同 InitRecord 初始化）。

### 启发式引擎评分构成（过渡方案）

| 分项 | 权重 | 依据 |
|:--|:--|:--|
| 时长匹配 | 60% | 音节数近似估计的期望时长 vs 实际话音时长（高斯衰减） |
| 话音占比 | 25% | 20ms 帧 RMS 活跃帧占比（阈值 = 峰值 -26dB） |
| 无削顶 | 15% | |幅值|>32000 采样占比越低越好 |

## 3. 固件侧设计（已实现，待 ES8311 实机）

### 3.1 接线（ES8311+NS4150B CODEC 模块，2026-08-24 定稿）

ES8311 CODEC 模块同时承担播放（DAC→NS4150B 功放）和录音（板载/FPC 模拟麦→ADC），
I2C 寄存器配置走 GPIO38/39，I2S 数据走 GPIO4/5/6/11（与播放共享总线）：

| 模块信号 | 接 ESP32-S3 | 说明 |
|:--|:--|:--|
| SDA | GPIO38（`ES8311_I2C_SDA_PIN`） | I2C 数据（板载 2.2k 上拉） |
| SCL | GPIO39（`ES8311_I2C_SCL_PIN`） | I2C 时钟 |
| SCLK | GPIO4（`I2S_BCK_PIN`） | I2S 位时钟（播放/录音共享） |
| LRCK | GPIO5（`I2S_WS_PIN`） | I2S 字选择 |
| DIN | GPIO6（`I2S_DATA_OUT_PIN`） | MCU→codec DAC 数据 |
| DOUT | **GPIO11（`I2S_DATA_IN_PIN`）** | codec ADC→MCU 数据；前置条件 BS 省线 |
| 5V | 5V（或 3V3，功率稍小） | 模块供电 |
| GND | GND | 共地 |

> **MCLK 省线方案**：不接 MCLK 线，ES8311 REG01 bit7=1 选 SCLK 作内部主时钟源
> （esp-adf LyraT-Mini 同款）；固件 `ES8311_MCLK_PIN=-1`。
> **NS4150B CTRL**：板载 R10 上拉常开，无 MCU 控制线（关功放需挪 R10→R11 焊盘）。
> **勘误**：早期建议的 GPIO7 已被 `EPD_SCK_PIN` 占用；INMP441 方案已废弃
> （ES8311 ADC 取代）。见 `gpio_config.h` ES8311 段 + 录音段注释。

### 3.2 I2S 全双工录音

- 现有 `audio_player.c`（`I2S_NUM_0`，TX-only 44.1kHz）扩展为按需切换：
  录音期重配 **16kHz/16bit/mono、TX|RX 全双工**，`data_in_num = SD 引脚`。
- **全双工纪律**：录音期间必须持续向 TX 写静音零样本（每读一批写一批零），
  否则部分功放/时钟组合下产生杂音或 DMA 异常。
- 录音结束恢复播放配置（现有 `i2s_configure_std(44100, ...)` 复用）。

### 3.3 录音流程（已实现：中键听-跟一体流）

1. 词卡中键（speak）→ 播单词音频；云端词播完自动进跟读（pron_task
   等播完上限 8s，本地词零打扰仅播放）→ 屏显 "Speak now"。
2. 录音最长 3s（`max_ms=3000`，1000~10000 可调——P2B 对话放宽 10s
   /320044B PSRAM 复用同一录音本体）；尾端静音 ≥800ms（块能量低于
   -35dBFS）提前结束；从未检测到话音返回 -3，屏显「未听到」免无意义上传。
3. 采样缓冲置于 PSRAM（≤96KB，词池预算外临时分配，录完即释放）。
4. 就地构造 44B 标准 WAV 头（RIFF/fmt 16/data 块）+ PCM 数据（32bit
   槽 >>16 截取，ES8311 ADC 16bit 有效数据居 32bit 槽高位）。
5. HTTP `POST` multipart 三段流式上传（头/体/尾分写，免整包 PSRAM
   拼装），wordId 取当前 `WordEntry.cloud_id`，X-Device-Key 鉴权。
6. 解析 `data.total`：**≥60 → 两短震（HAPTIC_PASS），<60 → 一长震
   （HAPTIC_FAIL）**；屏显分数与 engine 标识（heuristic 显示「基础
   评分」角标，gop 显示「精细评分」）。
7. 网络失败：**丢弃本次不缓存**（3s WAV 重录成本低于缓存复杂度，
   2026-08-24 计划裁定；偏离本节早期「本地缓存重传」方案，注释在
   `study_mode_machine.c` pron_task）。任务化不阻塞按键：任意键取消
   录音/关结果屏。

### 3.4 固件不做的事

- 不做任何评分/识别（AI 后端化红线）。
- 不压缩音频（拒绝 OPUS：3s/16kHz WAV ≈96KB 局域网无压力，见方案评审记录）。
- 不新增 WordEntry 字段（评分仅上行，设备端不留存历史分）。

## 4. 实施前置条件

- [x] ES8311+NS4150B CODEC 模块驱动预写（§3.1，es8311.c/h + audio_player/mic_recorder 接入，8 env 构建通过；待模块到货实机验证）
- [x] `gpio_config.h` 增加 `I2S_DATA_IN_PIN`（GPIO11，含 §3.1 勘误注释）
- [x] 新建 `src/mic_recorder.c/h`（全双工配置 + 采集 + WAV 组装；2026-08-24
      八 env 构建绿，参数化 `max_ms`/`send_now` 支撑 P2B 复用）
- [x] 词卡 UI 状态机加「跟读中/评分中/结果」三态（`ui_render_pron` +
      pron_task 听-跟一体流，含失败/无声态）
- [x] PRD V2.x P4 状态同步（本文件即设计交付物）

## 5. 升级路径：sherpa-onnx GOP（后端）

- 选型：sherpa-onnx（NuGet `org.k2fsa.sherpa.onnx`，runtime native 按 RID
  分发免装；GOP 音素评分 + 音素强制对齐，CPU 亚秒级）；备选 whisper.cpp
  （词级时间戳，弱于音素级）。
- 集成：后端镜像集成 native 库 + 模型文件（~40MB volume 挂载，不入镜像层）；
  `PronunciationService.AssessAsync` 内部按模型可用性切换引擎，`engine` 字段
  如实上报；协议/固件零改动。
- **P2A 已先行集成 zipformer 英语 ASR 模型**（`AsrService.cs`，同一 NuGet
  绑定与 volume 挂载模式，见 `docs/AI_CHAT_MODE.md` §3）——GOP 升级时
  绑定/挂载基建直接复用，仅换评分模型与 AssessAsync 内部分支。
- 验收：`spectacular`/`spectacle` 类易混词区分度抽测（GOP 能分、heuristic 不能）。
