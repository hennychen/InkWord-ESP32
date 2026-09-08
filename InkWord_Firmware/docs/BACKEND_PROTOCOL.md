# InkWord 设备端后端 API 协议

设备固件与云端后端之间的 HTTP 协议全清单，供**自建后端 / 私有部署**实现
参照（开源通用化 Phase 3，2026-10-24）。每节标注固件侧权威源码位置；
实现自建后端时以代码为准，本文与代码不一致属文档缺陷。

离线单机档（`env:inkword-s3-offline`）不依赖任何本协议端点，见
README「离线单机模式」节。

---

## 1. 通用约定

### 1.1 Base URL 配置层级（先先生效）

| 层级 | 机制 | 权威源码 |
|---|---|---|
| 默认 | `https://api.einkword.com` | sync_client.c `s_base_url` |
| 构建期 | `-D INKWORD_API_BASE="..."` 宏（NVS 缺失时兜底） | sync_session.cpp `sync_credentials_load` |
| 运行期 | NVS 命名空间 `inkword` 键 `api_url` | 同上 |

设备不提供界面改 Base URL；私有部署用构建期宏或 LAN 配网写入 NVS。

### 1.2 认证：`X-Device-Key` 头

- 除**注册**外，所有端点携带请求头 `X-Device-Key: {apiKey}`；
- apiKey 来自注册响应（§2.1），持久化于 NVS `inkword`/`dev_key`；
- JSON 端点同时携带 `Content-Type: application/json`。

### 1.3 传输分流（固件侧约定）

URL 以 `http:`（明文）开头 → 直接 TCP；其余（如 `https:`）→ TLS +
证书包（`esp_crt_bundle`）。**自建后端在局域网跑 HTTP 即可被设备访问**，
无需配证书。权威实现：sync_client.c `fill_cfg`（chat/voice/ota/audio_sync
四处同款复制）。

### 1.4 响应信封与错误语义

多数端点返回统一信封：

```json
{ "code": 0, "message": "...", "data": { ... } }
```

- `code` 数字，`0` 成功，非 0 业务失败；
- HTTP 状态码语义（固件按此分支，自建后端必须遵守）：

| HTTP | 固件行为 | 备注 |
|---|---|---|
| 200 | 解析信封 | |
| 401 | `SYNC_ERR_AUTH`：清 NVS+内存钥，按 MAC 幂等重注册自愈（§2.3） | App 绑定设备后云端换发 ApiKey 的对账机制 |
| 404 | `SYNC_ERR_DROP`：**永久失败，丢弃事件**防上报队列队头阻塞 | 仅 master/reading-progress/bookmarks 端点；后端未部署窗口期约定 |
| 其他/超时 | 临时失败，保留队列下周期重试 | |

### 1.5 时间戳与 wordId

- `wordId`：云端 Guid 字符串（36 字符）。本地导入词无 cloud_id，
  上报事件直接丢弃（sync_session.cpp `sync_flush_pending`）；
- 学习事件 `timestamp` 恒传 0：设备无绝对时间域，由服务器落地时间
  代替（sync_client.c `sync_push_progress` 注释）。

---

## 2. 设备生命周期

### 2.1 注册 `POST /api/device/register`

MAC 幂等：同 MAC 重复注册返回**既有记录**的 apiKey（换钥自愈依赖
此语义）。设备侧 STA MAC 大写 12 hex（如 `AA BB CC DD EE FF` →
`AABBCCDDEEFF`）。

- 请求：`{ "mac": "AABBCCDDEEFF", "name": "" }`（name 可空）
- 响应：`{ "code": 0, "data": { "deviceId": "...", "apiKey": "..." } }`
  （固件只消费 `apiKey`）
- 超时 15s；权威源码：sync_client.c `sync_register`。

### 2.2 心跳 `POST /api/device/heartbeat`

每 10 分钟（后台任务）/ 每次静默心跳会话（RTC TIMER 唤醒的极简路径）。

- 请求：`{ "battery": 87, "version": "1.x.x" }`（电量百分比整数；
  version 为固件版本字符串）
- 响应：200 即可，信封 data 不消费
- 401 触发换钥自愈；权威源码：sync_client.c `sync_heartbeat`，
  编排 sync_session.cpp `background_task` / `silent_heartbeat_session`。

### 2.3 换钥自愈（401 → 重注册）

固件任何端点收到 401 后：清内存+NVS 钥 → 立即按 MAC 重注册
（幂等取回新钥）→ 回写 NVS。网络不可达时保持空钥下周期再试；
本地学习链路不依赖钥（离线优先红线）。
权威源码：sync_session.cpp `sync_recover_auth`（ADR-001 §五）。

### 2.4 OTA 检查 `GET /api/device/ota/check`

**无信封**，直接顶层 JSON：

```json
{ "hasUpdate": true, "url": "https://.../fw.bin", "md5": "...", "size": 2400000 }
```

- `hasUpdate` 为 false 时其余字段缺省；
- `url` 为固件 bin 的直接下载地址（esp_https_ota 流式烧写）；
- 服务器按 X-Device-Key 识别设备当前版本（无 query 参数）；
- 权威源码：ota_manager.c `ota_check_for_update`。

---

## 3. 词库与学习同步

### 3.1 拉词库 `GET /api/device/sync/words?version={N}`

- `version`：设备本地词库版本号（整数）；
- 200 响应：**词库 JSON 全量**（word_parser 解析格式），响应头
  `X-Word-Version: {newVersion}` 携带新版本号；无新版时 body 可空、
  头返回当前版本；
- 头缺失时固件按「有数据 = N+1，无数据 = N」兜底；
- 权威源码：sync_client.c `sync_pull_words`。

### 3.2 学习进度 `POST /api/device/sync/progress`

- 请求：数组（当前每轮单条）
  `[ { "wordId": "<guid>", "quality": 3, "timestamp": 0 } ]`
- quality：FSRS 评分 0-5；
- 权威源码：sync_client.c `sync_push_progress`。

### 3.3 收藏 `POST /api/device/sync/collect`

- 请求：`{ "wordId": "<guid>", "collected": true }`
- 权威源码：sync_client.c `sync_push_collect`。

### 3.4 墨封（掌握标记）`POST /api/device/sync/master`

- 请求：`{ "wordId": "<guid>", "mastered": true }`
- **404 = SYNC_ERR_DROP**（后端未部署窗口期：丢弃事件防队头阻塞，
  NVS 已存终态，部署后由后续幂等 set 对账）；
- 权威源码：sync_client.c `sync_push_master`。

---

## 4. 音频

### 4.1 词条发音 `GET /api/device/audio/{cloudId}.mp3`

- `cloudId`：词条云端 Guid；响应体 MP3 流（单条约 4KB）；
- **404 = 云端尚未合成**（TtsJob 次日补齐），非错误不重试；
- 固件流式写 SD 临时文件 + rename 防半文件；单文件 >1MB 视为异常
  响应丢弃；连续 5 次网络级失败熔断本轮；
- 权威源码：audio_sync.c `download_one`。

### 4.2 对话回复音频

对话响应（老信封）的 `audioUrl` / 流式 `s` 行的 `u` 字段均为
**相对路径**（如 `/api/device/audio/chat_1760000000_s0.mp3`），设备
拼接 Base URL 后 GET 下载，带 `X-Device-Key`。句文件播完即删
（云端 CleanupChatClips 小时级回收）。
权威源码：chat_mode.c `chat_download_to`。

---

## 5. AI 语音对话 `POST /api/device/chat`

单端点串 ASR + LLM + TTS 全链路。请求 multipart/form-data，响应
**NDJSON 流式**（`?stream=1` 协商）或老式整包 JSON。

### 5.1 请求

```
POST {base}/api/device/chat?mode={mode}&scenarioId={id}&stream=1
X-Device-Key: {apiKey}
Content-Type: multipart/form-data; boundary=InkWordChat1886

--InkWordChat1886
Content-Disposition: form-data; name="file"; filename="chat.wav"
Content-Type: audio/wav

<WAV 16kHz/16bit/mono，≤10s>
--InkWordChat1886--
```

- `mode` / `scenarioId` 可选（场景对话；缺省 = 自由对话）；
- `stream=1`：流式协商位，老后端忽略未知 query 行为不变；
- 上传超时 45s（写阶段）；读流阶段改为 500ms 短超时回环。
- WAV 头 44 字节后按 16000Hz×2B 计算录音时长。

### 5.2 流式响应（NDJSON，每行一个 JSON 对象，`\n` 分隔）

按 `t` 字段分发（权威源码 chat_mode.c `stream_line_dispatch`）：

```jsonl
{"t":"meta","roundId":"<guid>","transcript":"识别文本","warmup":"场景首轮中文预热"}
{"t":"s","x":"回复句文本","u":"/api/device/audio/chat_..._s0.mp3"}
{"t":"s","x":"第二句","u":"..."}
{"t":"end","wordHits":[{"text":"apple","cloudId":"<guid>"}],"commands":[{"a":"replay"}]}
```

| 行类型 | 字段 | 设备行为 |
|---|---|---|
| `meta` | roundId（abort 用）/ transcript（识别回显，ASR 后即发）/ warmup（可选，仅 scenario 首轮） | 缓存 |
| `s` | x=句文本（必填）；u=句音频相对路径（可空 = TTS 降级纯文本句） | 句音频**下载即播**（首句起播不等 end）；x 驻留屏显逐句覆盖 |
| `end` | wordHits 可选 `[{text,cloudId}]` ≤5 条生词命中；commands 可选 `[{a:"replay"}]` 重播指令；emotion 预留不消费 | 续播至完；replay = 句文件保留重播 |
| `err` | — | 整轮失败（net_fail 语义） |

- 后端护栏 ≤6 句；无 `end` 的 EOF = 异常截断按失败处理；
- 首行无 `t` 字段 = **老后端整包 JSON 回退**（见 5.3）。

### 5.3 老式整包响应（兼容路径）

```json
{ "code": 0, "data": { "reply": "全文回复",
  "audioUrl": "/api/device/audio/chat_{ts}.mp3", "warmup": "...",
  "wordHits": [ {"text": "...", "cloudId": "..."} ] } }
```

`audioUrl`/`warmup`/`wordHits` 均可选，缺省降级不熔断
（audioUrl 空 = TTS 失败仅屏显文本）。
权威源码：chat_mode.c `chat_parse_legacy`。

### 5.4 打断上报 `POST /api/device/chat/abort?roundId={id}`

用户打断（barge-in）时 fire-and-forget 上报（3s 超时失败忽略）；
未收到 meta（无 roundId）不发。关流本身也会触发后端
RequestAborted 截断，本调用是反向代理不传递断连场景的显式保底。
权威源码：chat_mode.c `chat_abort_post`。

### 5.5 barge-in 时序约定（设备侧）

- 读流期以 500ms 短超时回环查询打断位（拾起延迟 ≤0.5s）；
- 打断 = 关流 + 停播 + 删已落地句文件 + abort 上报 + 静默进新轮
  （新轮开头旧打断位作废）。

---

## 6. 语音查词 `POST /api/device/voice-search`

### 6.1 请求

```
POST {base}/api/device/voice-search?deck={deckId}
（multipart 同 §5.1，filename="vs.wav"，WAV ≤3s，上传超时 20s）
```

`deck`：当前词书 id（三级词库匹配范围）。

### 6.2 响应

```json
{ "code": 0, "data": { "transcript": "识别文本",
  "candidates": [ { "text": "apple", "meaning": "苹果（释义≤60字符，后端截断）", "cloudId": "<guid>" } ] } }
```

候选 ≤5 条（top-5 匹配）。权威源码：voice_search.c `vs_upload`
（信封解析见 288-301 行段）。

---

## 7. 阅读器同步

### 7.1 阅读进度 `POST /api/device/sync/reading-progress`

```json
{ "bookKey": "book-id-or-hash", "signature": 1234567890,
  "currentPage": 42, "totalPages": 300, "fontLevel": 1, "readMinutes": 15 }
```

- `signature`：书内容签名（uint32，防同 key 换版本书进度错乱）；
- **404 = SYNC_ERR_DROP**；权威源码：sync_client.c
  `sync_push_reading_progress`。

### 7.2 书签全量同步 `POST /api/device/sync/bookmarks`

```json
{ "bookKey": "...", "signature": 1234567890,
  "bookmarks": [ { "page": 10, "byteOffset": 2048, "note": "" } ] }
```

- **全量覆盖**语义（非增量）；**404 = SYNC_ERR_DROP**；
- 权威源码：sync_client.c `sync_push_bookmarks`。

### 7.3 书架列表 `GET /api/device/books`

200 响应体为书籍列表 JSON（固件拉回后按自定结构解析展示）。
权威源码：sync_client.c `sync_pull_book_list`。

### 7.4 书籍下载 `GET /api/device/books/{bookKey}/download`

200 响应体为书文件原始字节流（固件流式写 SD）。下载超时 30s。
权威源码：sync_client.c `sync_download_book`。

---

## 8. 待机页数据

### 8.1 天气 `GET /api/device/weather`

```json
{ "code": 0, "data": { "icon": 2, "tempC": 18, "desc": "多云",
  "serverTime": 1760000000, "tzOffsetMin": 480 } }
```

- 后端聚合上游天气并缓存；`icon` 数字枚举、`tempC` 整数℃；
- `serverTime`/`tzOffsetMin` 为校时辅助（int64 Unix 秒 / int16 分钟）；
- 固件每约 30 分钟拉一次（仅待机页激活时）；
- 权威源码：sync_client.c `sync_fetch_weather`。

### 8.2 对话周报 `GET /api/device/chat-review`

```json
{ "code": 0, "data": { "weekStart": "2026-10-19...", "turnCount": 23,
  "review": { "summary": "...", "topics": [], "highlights": [],
    "suggestion": "...", "reviewWords": ["apple", "banana"] } } }
```

- 设备只消费 summary/suggestion/reviewWords（≤5 词屏显）；
- **404 = 返回 1**（周报未生成，屏显文案与网络失败区分）；
- 权威源码：sync_client.c `sync_fetch_chat_review`。

---

## 9. 校时 `HEAD http://connect.rom.miui.com/generate_204`

设备主时间源（替代被运营商 UDP 123 劫持废掉的 SNTP）：解析响应
`Date` 头（RFC1123 格式，合法窗 2025-01-01 ~ 2100-01-01）为 Unix 秒。
仅 2xx/3xx 状态被接受。权威源码：sync_client.c `sync_fetch_http_time`。

自建后端**无需实现**校时（设备直连公共探测地址）；天气端点的
serverTime 字段是辅助校时源。

---

## 10. 自建后端最小实现清单

离线学习档之外的**联网功能**逐项所需端点：

| 功能 | 必须实现 | 备注 |
|---|---|---|
| 词库云端同步 | register + sync/words + sync/progress(+collect/master) | master 可 404 降级 |
| 词发音下载 | audio/{cloudId}.mp3 | 未合成可 404 |
| AI 对话 | chat（整包 JSON 即可，流式可选）+ audio/* | abort 可空实现 |
| 语音查词 | voice-search | |
| 阅读器同步 | reading-progress/bookmarks/books/download | progress/bookmarks 可 404 降级 |
| 待机页天气/周报 | weather / chat-review | 周报可 404 |
| OTA | ota/check + 静态文件托管 | 可返回 hasUpdate:false |

全部端点汇总：`register / heartbeat / ota/check / sync/words /
sync/progress / sync/collect / sync/master / sync/reading-progress /
sync/bookmarks / books / books/{key}/download / audio/{id}.mp3 /
chat / chat/abort / voice-search / weather / chat-review`。

---

## 11. LAN 直传服务（设备侧 server，非本协议）

设备联网后监听 LAN HTTP 服务（词书直传 / 屏幕镜像 / Wi-Fi 配置 /
课程表编辑），端点清单见 `src/lan_display_server.cpp` 头注释
（T2.3 协议 v2）；与本文档的云端协议相互独立。
