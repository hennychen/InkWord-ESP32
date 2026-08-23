# AI 语音跟读评测（路径 C）设计文档

> 状态：**协议已冻结，后端已上线（M5.1）**；固件侧待 INMP441 麦克风接线后实施（M5.2）。
> 关联：PRD V2.1 P4「语音跟读评分」；后端 `PronunciationService.cs` / `DeviceController.Pronunciation`。

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

## 3. 固件侧设计（待硬件）

### 3.1 接线（INMP441 省线方案）

复用现有 I2S 播放时钟线（PRD 4.2 注），仅新增 1 根数据线：

| INMP441 引脚 | 接 ESP32-S3 | 说明 |
|:--|:--|:--|
| VDD | 3V3 | — |
| GND | GND | — |
| SCK | GPIO4（`I2S_BCK_PIN`） | 与播放共享位时钟 |
| WS | GPIO5（`I2S_WS_PIN`） | 与播放共享字选择 |
| SD | **GPIO 新增（建议 GPIO7）** | 麦克风数据 → ESP32 DIN |
| L/R | GND | 固定左声道 |

### 3.2 I2S 全双工录音

- 现有 `audio_player.c`（`I2S_NUM_0`，TX-only 44.1kHz）扩展为按需切换：
  录音期重配 **16kHz/16bit/mono、TX|RX 全双工**，`data_in_num = SD 引脚`。
- **全双工纪律**：录音期间必须持续向 TX 写静音零样本（每读一批写一批零），
  否则部分功放/时钟组合下产生杂音或 DMA 异常。
- 录音结束恢复播放配置（现有 `i2s_configure_std(44100, ...)` 复用）。

### 3.3 录音流程（词卡界面触发）

1. 用户在词卡长按（或既定按键）进入跟读模式 → 屏显 "Speak now"。
2. （可选）播放单词音频一遍（现有 audio_player），播完启动录音。
3. 录音最长 3s；尾端静音 ≥800ms（帧能量低于 -35dB）提前结束，节省等待。
4. 采样缓冲置于 PSRAM（≤96KB，词池预算外临时分配，录完即释放）。
5. 就地构造 44B 标准 WAV 头（RIFF/fmt 16/data 块）+ PCM 数据。
6. HTTP `POST` multipart 上传（复用 `sync_client.c` 的 HTTP 客户端与设备鉴权头），
   wordId 取当前 `WordEntry.cloud_id`。
7. 解析 `data.total`：**≥60 → 两短震（`haptic_pulse2`），<60 → 一长震（`haptic_pulse`）**；
   屏显分数与 engine 标识（heuristic 显示「基础评分」角标）。
8. 网络失败：本地缓存待联网重传（复用学习记录队列思路），不阻塞学习主流程。

### 3.4 固件不做的事

- 不做任何评分/识别（AI 后端化红线）。
- 不压缩音频（拒绝 OPUS：3s/16kHz WAV ≈96KB 局域网无压力，见方案评审记录）。
- 不新增 WordEntry 字段（评分仅上行，设备端不留存历史分）。

## 4. 实施前置条件

- [ ] INMP441 模块焊接接线（§3.1，外部硬件事件）
- [ ] `gpio_config.h` 增加 `I2S_DATA_IN_PIN`
- [ ] 新建 `src/mic_recorder.c/h`（全双工配置 + 采集 + WAV 组装）
- [ ] 词卡 UI 状态机加「跟读中/评分中/结果」三态
- [ ] PRD V2.x P4 状态同步（本文件即设计交付物）

## 5. 升级路径：sherpa-onnx GOP（后端）

- 选型：sherpa-onnx（C++，有 .NET 绑定与预编译模型；GOP 音素评分 + 音素
  强制对齐，CPU 亚秒级）；备选 whisper.cpp（词级时间戳，弱于音素级）。
- 集成：后端镜像集成 native 库 + 模型文件（~40MB volume 挂载，不入镜像层）；
  `PronunciationService.AssessAsync` 内部按模型可用性切换引擎，`engine` 字段
  如实上报；协议/固件零改动。
- 验收：`spectacular`/`spectacle` 类易混词区分度抽测（GOP 能分、heuristic 不能）。
