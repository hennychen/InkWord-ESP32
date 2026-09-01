# InkWord-ESP32（墨读 InkRead）

基于 ESP32-S3 墨水屏的英语单词学习系统 —— 全栈 IoT 解决方案：固件（设备端）+ 后端 API + 管理后台。

> 本文档是项目**现状快照与升级基线**（2026-08-21 整理）。后续升级以此为准，配合 `docs/PRD_V2.1.md`（需求全集）与 `docs/WIRING_DIAGRAM.md`（接线全集）使用。

---

## 1. 项目状态快照（2026-08-22）

### 1.1 总体结论

**V2.1 软件开发生命周期已完成 + AI 辅助学习三路径接入**：三端可构建、可运行、已通过本机验证；项目进入**硬件联调阶段**。剩余工作分三类（详见 §11 升级路线图）：

| 类别 | 含义 | 数量 |
|:---|:---|:---:|
| 待上机验证 | 代码已完成且构建通过，等待硬件在手烧录验证 | 4 项 |
| 待硬件接线 | 固件就绪，等元件/接线（功放+喇叭、马达、SD 卡） | 3 项 |
| V2.x 规划 | 深睡功耗、BLE 配网等下一代特性 | 6 项 |

**AI 辅助学习接入（2026-08-22，三路径；2026-08-24 音频闭环 + 对话模式）**：

| 路径 | 内容 | 状态 |
|:---|:---|:---:|
| B：LLM 词库增强 | Ollama（可切云 API）批量生成分级例句/词根助记/易混辨析，管理端 diff 审核后写入（防幻觉红线） | ✅ 后端+前端 |
| A：FSRS 替换 SM-2 | 后端 FsrsService 影子运行 + 对比看板；固件 srs_engine 已换 FSRS-4.5（LR03），双端共享对拍向量全绿 | ✅ 影子运行中 |
| C：语音跟读评测 | 发音评测端到端（固件听-跟一体流 + heuristic 引擎，GOP 升级路径已就绪）；待 ES8311 实机（docs/AI_SPEECH_ASSESSMENT.md） | ◐ 待实机 |
| 发音音频闭环 | Piper TTS 批量合成 + 按需下发 + SD 自动同步（P0A/B/C；中键真实发音，libhelix MP3 异步播放） | ✅ 代码完成（待硬件实机） |
| AI 语音对话 | ASR(sherpa-onnx)+LLM+TTS 单端点编排（P2A）；固件 MODE_CHAT 五态状态机、语音优先/三色屏降级（P2B）；P0-1 流式化：NDJSON 句级流水线 + barge-in abort + replay 命令 + 会话摘要压缩（docs/AI_CHAT_MODE.md §2b） | ◐ 待 ES8311 实机 |

### 1.2 里程碑（对照 PRD §九）

| 阶段 | 交付物 | 状态 |
|:---|:---|:---:|
| P1 硬件验证 | 屏幕点亮、SPI、五向导航 | ✅ |
| P2 基础固件 | 全刷/局刷、按键框架、音频框架 | ✅ |
| P3 核心功能 | 三学习模式 + SRS + 刷新调度 + 错词本 + 词库 4000 词扩容 + 阅读模式（待上机） | ✅ 代码完成 |
| P4 云端后台 | API + 管理端五模块，2026-08-20 本机全链路验证通过 | ✅ |
| P5 联调与优化 | OTA/残影调优进行中；功耗优化未启动（依赖深睡架构） | ◐ |
| P6 公测与迭代 | 词库填充、体验优化 | 持续 |

### 1.3 三端构建/验证状态

| 端 | 构建命令 | 状态 | 最近验证 |
|:---|:---|:---:|:---|
| 固件（8 面板构建矩阵） | `pio run`（8 env：生产/演示/三色/BW 骨架） | ✅ | 2026-08-24（Flash 76.9%；libhelix MP3 + audio_sync + mic_recorder + chat_mode 全量纳入） |
| 固件算法单测（host） | `pio test -e native-test` | ✅ 2/2 | 2026-08-24（FSRS 对拍向量 + 锚点；mp3/chat 不入 filter 零污染） |
| 固件音标修复（IPA 点阵） | clean 全量零警告 + wf0270 抽查 + native-test 2/2；真机 2407 词条 `/səˈsaɪəti/` 重音符/斜杠直渲（详见 InkWord_Firmware/README.md） | ✅ | 2026-08-24 |
| 后端 | `dotnet build`（0 错误）＋ `dotnet test` 39/39 ＋ 本机运行冒烟（登录/影子链路/发音评测/AI 失败标记与复位闭环） | ✅ | 2026-08-24（P0B/P2A 增 Tts/Chat/Asr 编排与端点测试） |
| M1 AI 真实生成验收 | Ollama qwen2.5:1.5b（brew 原生，Metal）：生成→待审→应用→驳回→导出全链路；Example≤108B / Root≤20B 红线合规 | ✅ | 2026-08-23 |
| 云端 LLM 接入 | Provider=openai + CloudEndpoint（DeepSeek/Qwen/智谱三家实测）；kind=2 辨析链路通过 | ✅ | 2026-08-23 |
| 管理后台 UI 实测 | 登录/看板对比图/词表 AI 徽标（含失败+复位交互）/生成弹窗/审核台空态浏览器全过 | ✅ | 2026-08-23 |
| 管理后台 | `ng build`（16.3s 零警告）＋ AI 审核台/对比图表 | ✅ | 2026-08-22 |

---

## 2. 实物照片

| 2.7 寸黑白红墨水屏（显示中文引文） | 4.2 寸 GOODISPLAY 黑白红墨水屏（400×300） |
|:---:|:---:|
| ![2.7寸黑白红墨水屏](Info/2.7寸黑白红墨水屏.jpg) | ![4.2寸黑白红墨水屏](Info/4.2寸黑白红墨水屏.jpg) |

---

## 3. 项目结构

```
InkWord-ESP32/
├── InkWord_Firmware/        # ESP32-S3 固件（Arduino 框架 + ESP-IDF 组件，24 个 C/C++ 模块）
│   ├── src/                 #   源码（main.cpp + 各功能模块，详见 §5.1）
│   ├── tools/               #   字库/图标生成脚本（gen_cjk_font.swift / gen_weather_icons.py）
│   ├── partitions_default.csv
│   └── platformio.ini       #   inkword-s3（生产）/ inkword-s3-demo（演示，内嵌词库+演示书）
├── InkWord_Backend/         # 后端 API（.NET 8 五项目分层）
│   └── src/
│       ├── InkWord.API/         # 控制器/DTO/中间件/Program.cs
│       ├── InkWord.Core/        # 实体/接口/公共类型
│       ├── InkWord.Infrastructure/  # EF Core DbContext/仓储/Redis 缓存
│       ├── InkWord.Services/    # SRS/FSRS/AI 内容/发音评测等领域服务
│       └── InkWord.Jobs/        # Hangfire 定时任务（含 AI 夜间批量生成）
├── InkWord_Admin/           # 管理后台（Angular 17 standalone + Material + ECharts）
│   └── src/app/
│       ├── auth/            #   登录页
│       ├── layout/          #   主布局（侧边导航/工具栏/三态主题切换）
│       ├── core/            #   服务（认证/主题/Token）+ API 服务 + 拦截器 + 模型
│       └── modules/         #   dashboard / word-management（含 AI 审核台）/ device-management / ota / wrongbook
├── tools/                   # 跨端工具（gen_fsrs_vectors.py：FSRS 对拍向量双输出 CSV+固件头）
├── docs/
│   ├── PRD_V2.1.md          # 产品需求全集（功能树/接口契约/里程碑/风险）
│   ├── AI_SPEECH_ASSESSMENT.md # 语音跟读评测协议与固件设计（M5 路径 C）
│   ├── WIRING_DIAGRAM.md    # 硬件接线全集（GPIO 分配/演变速记）
│   └── PANEL_COMPAT_DESIGN.md # 多屏兼容设计（面板描述符/五层架构/色彩平面/迁移路径）
├── docker-compose.yml       # 全栈编排（postgres/redis/ollama/backend/frontend）
└── README.md                # 本文档
```

---

## 4. 系统架构

```
┌─────────────────────── ESP32-S3 N16R8（墨读设备）───────────────────────┐
│  墨水屏 DEPG0370 240x416 ── GxEPD2_374_DEPG0370 + epd_gfx_*（横屏 416x240）│
│  五向导航开关 + SET/RST 侧键 ── button_handler                           │
│  四学习模式（study_mode_machine）：闪卡 / 三选一 / 拼写 / 阅读（reader_engine）│
│  SRS 引擎（srs_engine，FSRS-4.5）+ 学习状态 LR03 sparse NVS（learning_state） │
│  词池 4000 词 PSRAM（word_parser）│ 三级点阵字库 3892 字（cjk_font+cjk_text） │
│  待机页（standby_page）│ 防残影刷新调度（refresh_scheduler）│ OTA（ota_manager）│
│  Wi-Fi 管理 + SoftAP 配网门户 + LAN 网页直传（wifi_manager/lan_display_server）│
│  BLE 配网（ble_provision，待 coex 评估）│ SD 书籍/音频（storage_manager）      │
└──────────────┬──────────────────────────────────────────────┘
               │ HTTPS/JSON（X-Device-Key 认证）
               ▼
┌─────────────────────── InkWord_Backend（.NET 8）───────────────────────┐
│  设备 API：register / sync/words（增量词库）/ sync/progress / heartbeat /  │
│           ota/check                                                      │
│  管理 API：auth/login（JWT）│ words CRUD + CSV 11 列导入 + export          │
│           devices + 远程指令 │ ota 包管理 │ dashboard 统计/错词排行/日活     │
│  PostgreSQL 16（Words 含 Root/Inflections/Source/Grade 溯源四字段、       │
│   Devices、LearningRecords 含 ConsecutiveWrong/IsCollected、OTA、User）   │
│  Redis 7（缓存）│ Hangfire（每日推送/冷词归档）│ Serilog                    │
└──────────────┬──────────────────────────────────────────────┘
               │ REST/JSON（JWT Bearer）
               ▼
┌─────────────────────── InkWord_Admin（Angular 17）─────────────────────┐
│  数据看板（统计卡片+ECharts 日活/SRS 分布）│ 错词排行                       │
│  词库管理（CRUD/CSV 导入导出/11 字段表单）│ 设备管理 │ OTA 升级              │
│  三态主题（亮/暗/跟随系统）+ 首帧防闪 + JWT 拦截器 + 错误拦截器               │
└──────────────────────────────────────────────────────┘
```

---

## 5. 技术栈

### 5.1 固件（`InkWord_Firmware/`）
- **芯片**: ESP32-S3 N16R8（16MB Flash + 8MB Octal PSRAM；Arduino 框架 + ESP-IDF 组件，PlatformIO espressif32@7.0.1）
- **屏幕**: DKE DEPG0370 3.7" 240x416（UC8253 类 COG）+ EVK011 升压转接板；自有面板类 `GxEPD2_374_DEPG0370`（PSR=0xDF、全刷 CDI=0x97、局刷 CDI=0x17）+ `epd_gfx_*` C 薄适配层；双坐标体系（底层竖屏直通 / GFX 层横屏）
- **内存布局**（8MB PSRAM 三层共享）: 词池 4000 词×1096B（逐半降级兜底）＋ 词库 JSON 缓冲 2MB ＋ 阅读器书文件 ≤4MB（最坏 8.2MB 仅"满词库+大书"同时存在触顶）＋ 学习状态数组 4000×24B
- **字体**: FreeSans 9/18/24pt（ASCII）+ 三级点阵中文字库 16/20/24px（3892 字/646KB bin 嵌入，Kaiti SC Bold 优先回退链，`tools/gen_cjk_font.swift` 生成）+ `cjk_text` 中英混排（CJK 按字断行 / ASCII 按词断行）
- **存储**: NVS（LR03 sparse 学习状态：非默认词 21B/条、脏标记 5s 延迟落盘）+ SD 卡 SPI+FAT（待接线）
- **音频**: I2S + ES8311+NS4150B CODEC（`audio_player.c`：WAV/MP3（libhelix 源码内嵌）异步任务队列播放，重按打断重播）＋ ES8311 ADC 录音（`mic_recorder.c`：全双工录音 3s 跟读/10s 对话双档）＋ 云端音频同步（`audio_sync.c`）
- **网络**: Wi-Fi STA + SoftAP 配网门户（captive portal）+ LAN 网页直传文本/图片（浏览器端转码 1bpp）+ HTTP 同步 + OTA 双分区

### 5.2 后端（`InkWord_Backend/`）
- .NET 8 WebAPI（5 项目分层）+ EF Core 8（EnsureCreated 自动建表，无 Migrations）+ Npgsql
- PostgreSQL 16 + Redis 7（StackExchange.Redis）+ Hangfire（每日复习推送/冷词归档）+ Serilog
- JWT Bearer（PBKDF2 10000 轮密码哈希；开发种子账号 admin/admin123）+ 设备 ApiKey（`X-Device-Key`）
- Swagger/OpenAPI；CSV 词库导入 11 列（text,phonetic,meaning,example,audio,tag,difficulty,root,inflections,source,grade）

### 5.3 管理后台（`InkWord_Admin/`）
- Angular 17（standalone components）+ Angular Material（墨水屏黑白主题，亮/暗/跟随系统三态）
- ECharts（ngx-echarts）；JWT 拦截器 + 全局错误拦截器；Lazy-load 路由

---

## 6. 固件模块清单（`src/` 24 模块）

| 模块 | 职责 | 状态 |
|:---|:---|:---:|
| main.cpp | 应用骨架/词池 PSRAM 管理/词卡 UI（释义点阵混排+词根行+溯源标签行） | ✅ 待上机（词卡新 UI） |
| study_mode_machine | 四学习模式状态机（闪卡/三选一/拼写/阅读） | ✅ 已上机（阅读待上机） |
| srs_engine | FSRS-4.5 间隔重复（M4 路径 A，双端对拍全绿） | ✅ 待上机（SM-2 版已上机验证） |
| learning_state | LR03 sparse NVS 学习状态（21B/词、5s 延迟落盘、FSRS 字段） | ✅（新格式待上机） |
| word_parser | 词库 JSON 解析（WordEntry 11 字段 1096B） | ✅ |
| reader_engine | TXT 电子书分页引擎（变宽排版/页表/进度记忆 NVS） | ◐ 待上机 |
| cjk_font / cjk_text | 三级点阵字库访问 + 中英混排通用层 | ◐ 待上机 |
| epd_driver / GxEPD2_374 | 屏驱 + GFX 横屏层（全刷/无窗口整屏双 RAM 差分局刷） | ✅ 已上机 |
| refresh_scheduler | 防残影调度（学习/阅读 N=8、待机 12 强制全刷） | ✅ 已上机 |
| standby_page | 待机页（时钟/天气/引文轮换） | ✅ 已上机 |
| button_handler | 五向导航 + 侧键事件框架 | ✅ 已上机 |
| wifi_manager / wifi_config_ui / ble_provision | STA/AP 配网门户/BLE 配网 | ✅ 已上机（BLE 待评估） |
| lan_display_server | LAN 网页直传（文本/图片） | ✅ 已上机 |
| sync_client / ota_manager | 云同步/OTA 双分区 | ✅（同步待真实云端联调） |
| storage_manager | SD 卡（书籍/音频/词库） | ◐ 待接线 |
| audio_player / haptic | I2S 音频/马达事件表 | ◐ 待接线 |
| debug_log / toolchain_stubs | 调试日志/工具链桩等辅助 | ✅ |

---

## 7. 快速开始

### 7.1 Docker 全栈部署

```bash
docker compose up -d --build
# 前端 http://localhost ；后端 http://localhost:8080/swagger ；PG 5432 ；Redis 6379
# Ollama http://localhost:11434（AI 词库增强，首次拉模型：
#   docker compose exec ollama ollama pull qwen2.5:7b ）
```

### 7.2 本地开发

#### 后端

> 前置：PostgreSQL 16+ 与 Redis（`docker compose up -d postgres redis`，或本机 brew 服务）。
> 无 EF Migrations：Development 启动时自动建表（EnsureCreated + Words 四列幂等补丁），
> 并种子默认账号 **admin / admin123**（仅 Development，生产需重置）。
> 建表必须在 Hangfire 初始化前执行（Hangfire 先建表会让 EnsureCreated 跳过建表）。
> 端口用 5090：**macOS AirPlay 接收器默认抢占 5000/7000**（Control Center 进程监听 *:5000，
> 浏览器走 IPv6 ::1 会连到它，现象为 CORS/连接异常；curl 走 IPv4 却正常，极具迷惑性）。

```bash
cd InkWord_Backend
export ASPNETCORE_ENVIRONMENT=Development
# 连接串按环境覆盖（默认 postgres/postgres；brew PG 为本机用户名免密）
export ConnectionStrings__Postgres="Host=localhost;Port=5432;Database=inkword;Username=<你的用户>"
dotnet run --project src/InkWord.API --urls http://localhost:5090
# API: http://localhost:5090/swagger（前端 environment.development 直连 5090）
```

##### LLM Provider 切换（Ai:Provider）

> key 只走环境变量 / dotnet user-secrets，**绝不入仓库**（appsettings*.json 均被 git 跟踪）。

| Provider | 环境变量 | 质量实测（词根） |
|:---|:---|:---|
| `ollama`（默认） | 无需 key；`Ai__OllamaUrl` / `Ai__Model` | 1.5b 差（`视→review 复习`）；7b 可用但慢 |
| DeepSeek ⭐ | `Ai__Provider=openai Ai__Model=deepseek-chat Ai__CloudEndpoint=https://api.deepseek.com/v1 Ai__CloudApiKey=<key>` | `re=再次; view=看` 最佳 |
| Qwen 云 | `Ai__Provider=openai Ai__Model=qwen-turbo Ai__CloudEndpoint=https://dashscope.aliyuncs.com/compatible-mode/v1 Ai__CloudApiKey=<key>` | `rev=回; view=看` 良好 |
| 智谱（免费档） | `Ai__Provider=openai Ai__Model=glm-4-flash Ai__CloudEndpoint=https://open.bigmodel.cn/api/paas/v4 Ai__CloudApiKey=<key>` | 可用，格式略生硬 |

2026-08-23 实测：DeepSeek deepseek-chat 走后端全链路（生成→审核→应用→导出 43B 合红线上）通过；
云端为批量词库生成首选（质量/速度/磁盘均优于本地小模型，公开教辅数据无隐私顾虑）。

#### 管理后台

```bash
cd InkWord_Admin
npm install
npx ng serve            # http://localhost:4200（admin / admin123）
```

#### 固件

> 需 Python 3.10+（macOS 用 Homebrew Python 3.11；首次构建前 `pip install intelhex pyelftools`）

```bash
cd InkWord_Firmware
# 双环境编译（生产 inkword-s3 / 演示 inkword-s3-demo 内嵌中文 demo 词库+演示书）
/opt/homebrew/bin/python3.11 -m platformio run -e inkword-s3 -e inkword-s3-demo
# 烧录演示固件一次看全：中文词卡（释义/词根/标签行）+ 阅读模式 + 四模式
/opt/homebrew/bin/python3.11 -m platformio run -e inkword-s3-demo -t upload --upload-port /dev/cu.usbserial-0001
# 串口监控（烧录前先退出，否则 port is busy）
/opt/homebrew/bin/python3.11 -m platformio device monitor --port /dev/cu.usbserial-0001 --baud 115200
```

### 7.3 硬件接线（ESP32-S3 → EVK011 J2，2026-08 实测）

屏幕 9 根线（按转接板丝印）：SCK=GPIO7、SDO=GPIO8、D/C=GPIO9、CS=GPIO10、BS=GPIO11、RES=GPIO13、BUSY=GPIO12、3V3→J2-16(VCI)、GND→J2-1（必须共地）。⚠️ BS 只能接 GPIO11（固件驱动 LOW）或板侧短接 GND，绝不可悬空。

外设：五向导航 UP/DOWN=GPIO1/2、LEFT/RIGHT=GPIO14/15、CENTER=GPIO21、SET/RST 侧键=GPIO42/40、COM→GND；音频 ES8311 CODEC I2C(38/39)+I2S(4/5/6/11)；马达=GPIO41（MOS 驱动）；SD=SPI3 GPIO16/17/18/47。详见 `docs/WIRING_DIAGRAM.md`。

---

## 8. API 端点概览

| 分组 | 方法 | 路径 | 说明 |
|:---|:---|:---|:---|
| 认证 | POST | `/api/auth/login` | JWT 登录（返回 username/token/expiresIn） |
| 设备 | POST | `/api/device/register` | 设备注册（MAC 换 ApiKey） |
| 设备 | GET | `/api/device/sync/words` | 增量同步词库（camelCase，含四溯源字段） |
| 设备 | POST | `/api/device/sync/progress` | 上报学习记录（含连错/收藏） |
| 设备 | POST | `/api/device/heartbeat` | 心跳（5 分钟在线窗口） |
| 设备 | GET | `/api/device/ota/check` | OTA 检查 |
| 管理 | CRUD | `/api/admin/words` | 词库 CRUD（11 字段） |
| 管理 | POST/GET | `/api/admin/words/import` `/export` | CSV 11 列导入 / JSON 导出 |
| 管理 | GET | `/api/admin/devices` ＋ `/{id}/command` | 设备列表/远程指令 |
| 管理 | GET/POST | `/api/admin/ota` ＋ `/upload` | 固件包管理 |
| 管理 | GET | `/api/admin/dashboard/stats` `/wrong-top` `/srs-distribution` `/daily-active` `/srs-comparison` | 看板统计/错词排行/SRS 分布/日活/SM-2 vs FSRS 影子对比 |
| 管理 | POST | `/api/admin/words/ai-generate` | AI 批量生成入队（kind 0 例句/1 词根/2 辨析，Hangfire 异步） |
| 管理 | GET/POST | `/api/admin/words/ai-pending[/count]` `/ai-apply/{id}` `/ai-reject/{id}` | AI 建议审核（人工 diff 比对通过后才落词库字段，防幻觉红线）；`ai-reject` 兼复位生成失败词（AiStatus 3→0 重入队列） |
| 设备 | POST | `/api/device/pronunciation` | 发音评测（WAV 16kHz/mono≤3s → 总分+音素明细，M5 路径 C） |
| 设备 | GET | `/api/device/audio/{file}` | 词条/chat 音频下发（Piper TTS 产物，流式返回，X-Device-Key） |
| 设备 | POST | `/api/device/chat` | AI 语音对话（WAV ≤10s/512KB → ASR+LLM+TTS 单端点 → reply/audioUrl，P2A；`?stream=1` NDJSON 句级流式，P0-1） |
| 设备 | POST | `/api/device/chat/abort?roundId=` | 对话轮中止（barge-in 截断 LLM/TTS，fire-and-forget，P0-1） |

> 统一响应包装 `ApiResponse{code=0 成功, message, data}`——前端判定 `code === 0`（非 200）。

---

## 9. 核心算法

### FSRS-4.5 间隔重复（srs_engine，M4 路径 A 2026-08-22）
```
质量分 q(0~5) → rating 四档（0-1 遗忘 / 2 困难 / 3-4 良好 / 5 简单）
S/D 双记忆指标更新（open-spaced-repetition FSRS-4.5 默认 17 参数）
目标留存 0.90 → interval = round(S)；q<3 等级归零，q>=3 递增至 5
双端同源：与后端 FsrsService 共享 tools/gen_fsrs_vectors.py 对拍向量
（pio test -e native-test 2/2 绿；dotnet test 4/4 绿）
```

### 防残影刷新调度（refresh_scheduler）
```
局刷计数 < 8（待机页 12）: 无窗口整屏双 RAM 差分局刷（<1s）
计数达阈值: 强制全刷清残影 → 归零
局刷路径统一走 epd_gfx_flush_window*（前置 refresh_gfx_before_partial 检查）
```

### LR03 sparse 学习状态（learning_state）
```
NVS blob = {magic "LR03", count, used, lr_sparse_t[used]}
仅存非默认词（stability>0/连错>0/已收藏），21B/词，LR_SPARSE_MAX=300
载荷换 FSRS 字段（stability/difficulty + next/last 双 delta 秒）
旧 LR02（SM-2）状态 magic 不匹配自动作废重建（保守策略）
评分/收藏仅置脏，主循环 maybe_save() 静默 5s 后写（按键零 NVS 阻塞）
```

---

## 10. 已知注意事项（升级前必读）

1. **macOS 5000 端口被 AirPlay 抢占**：后端开发端口固定 5090（浏览器走 IPv6 会连到 Control Center，curl 却正常）。
2. **EnsureCreated × Hangfire 顺序**：建表/种子块必须在 `JobRegistrar.Register()` 之前，否则库非空跳过建表（Words 缺失 42P01）。
3. **PG 幂等补列补丁**：`Program.cs` 中 Words 四字段 `ALTER TABLE IF NOT EXISTS`——引入 EF Migrations 后删除。
4. **抽象类不得注册为 DI 实现**（曾因 RepositoryBase<> 开放泛型注册启动即崩）。
5. **前端契约**：成功码 `code===0`；MDC 组件颜色覆盖须用 `--mdc-*` CSS token（不继承普通 color）。
6. **pio 全量重建勿接 grep 管道**（超时），输出重定向文件后台跑；`PATH="$HOME/.platformio/penv/bin:$PATH"`。
7. **C 源码多字节字符**不能作 char 字面量（如间隔号用 `'\xC2','\xB7'` 两字节）。
8. **cjk_text 与 reader_engine 排版原语为同源副本**：上机验证后应合并单点维护。

---

## 11. 升级路线图

### 11.1 待上机验证（代码已完成，烧 `inkword-s3-demo` 一次看全）
| 项 | 入口 | 验证点 |
|:---|:---|:---|
| 阅读模式（TXT 分页/翻页/字号缩放/进度记忆） | reader_engine.c | 中文显示、变宽排版无溢出、重启续读 |
| 词卡中文渲染（释义/词根/溯源标签行） | cjk_text.c + main.cpp | 混排断行、缺字占位框 |
| LR03 sparse 学习状态（FSRS 字段） | learning_state.c | 评分/收藏重启保持、NVS 容量；旧 LR02 状态自动作废重建 |
| haptic 事件表 | haptic.c | （需马达接线后） |
| 词条音频同步 | audio_sync.c | 菜单「音频同步」缺 N/总 M 徽标，后台串行下载，网络熔断 |
| 跟读评测（听-跟一体流） | mic_recorder.c + pron_task | 中键播完自动进跟读，三态屏 + 震动映射（待 ES8311 实机） |
| AI 对话模式 | chat_mode.c（MODE_CHAT） | 五态状态机 + 三模式二级菜单（自由/英中翻译/场景 6 选）：中键三义、SET 短按收藏生词，三色屏纯语音降级；P0-1 流式消费（NDJSON 读流 + 句级流水线播放 + barge-in 3s 拾起 + 老后端首行探测回退）；协议 docs/AI_CHAT_MODE.md §2b/§7 |

### 11.2 待硬件接线（固件就绪）
| 项 | 接线 | 固件入口 |
|:---|:---|:---|
| ES8311+NS4150B CODEC 模块 | I2C(38/39)+I2S(4/5/6/11)+5V | audio_player.c（WAV/MP3 异步队列播放）+ es8311.c（codec 驱动）+ mic_recorder.c（ADC 录音） |
| （已取代 MAX98357A+INMP441 双件套，模块已购，驱动预写完成） | — | — |
| 震动马达 | GPIO41 + MOS | haptic.c |
| SD 卡（书籍/音频/词库文件） | SPI3 GPIO16/17/18/47 | storage_manager.c |

### 11.3 V2.x 规划（PRD §九增量排期 P4-P6）
| 项 | 依赖/说明 |
|:---|:---|
| SoC 深睡 + 定时唤醒刷新 | 功耗指标兑现（<5µA 待机/≥15 天续航）；需与待机页引文轮换分模式定义 |
| I2S 全双工 + 语音跟读评测实机验收 | ES8311 模块到货接线后（§11.2）；固件/后端均已交付（docs/AI_SPEECH_ASSESSMENT.md §4 勾选），GOP 音素级升级路径基建已随 P2A 就绪 |
| 提示音 | 经 ES8311 DAC 播短样本（依赖模块接线） |
| BLE 配网主链路 | ble_provision.cpp 已有雏形；需 BLE/Wi-Fi coex 或 IDF 迁移评估 |
| 设备挑战-应答认证 | 现为静态 ApiKey |
| 学习分析按用户/时间筛选 | 后端 wrong-top/srs-distribution 已通，筛选维度 V2 加 |
| EF Migrations | 替换 EnsureCreated + 幂等补列补丁 |

### 11.4 P5 联调优化（进行中）
- OTA 端到端（真实云端 + 设备双分区回滚）
- 残影调优（局刷阈值/全刷波形 A/B）
- 启动时间专项测量（目标 <3s）

---

## 12. 文档索引

| 文档 | 内容 |
|:---|:---|
| `docs/PRD_V2.1.md` | 需求全集：功能树/硬件 BOM/GPIO 分配/接口契约/验收标准/里程碑/风险 |
| `docs/AI_SPEECH_ASSESSMENT.md` | 语音跟读评测：协议冻结（WAV 上行/JSON 评分）/ES8311 CODEC 接线/I2S 全双工纪律/sherpa-onnx 升级路径 |
| `docs/WIRING_DIAGRAM.md` | 接线全集：屏幕/按键/音频/马达/SD + 历史演变速记 |
| `docs/PANEL_COMPAT_DESIGN.md` | 多屏兼容设计：面板描述符/五层架构/色彩平面与调色板/构建矩阵/Phase 0-8 迁移路径 |
| `InkWord_Firmware/README.md` | 固件详解：模块/构建/刷新策略/词库扩容与内存布局 |
| `InkWord_Admin/`、`InkWord_Backend/` 各自 README | 端内细节 |

## License

Proprietary — LexInk
