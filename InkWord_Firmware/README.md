# InkWord Firmware

基于 ESP32-S3 的墨水屏英语单词学习终端固件，运行于 ESP-IDF v5.x + EPDiy V7 平台。

## 硬件方案

| 组件 | 型号 / 规格 | 接口 |
|------|-------------|------|
| 主控 | ESP32-S3 (16MB Flash) | — |
| 屏幕 | 9.7" 墨水屏 ED097TC2 (1200×825, 横屏) | EPDiy V7 并口 |
| 音频 | MAX98357A 功放 | I2S |
| 存储 | MicroSD 卡 | SPI + FAT |
| 按键 | 6 个轻触按键 (A~F) | GPIO 独立输入 |

## 引脚映射

完整定义见 [`src/gpio_config.h`](src/gpio_config.h)。

| 外设 | 引脚 | 说明 |
|------|------|------|
| **墨水屏** | | EPDiy V7 板内固定布线 |
| — BUSY | GPIO48 | EPD 状态检测 |
| — RESET | GPIO8 | EPD 硬复位 |
| **I2S 音频** | | 标准飞利浦 I2S |
| — BCLK | GPIO4 | 位时钟 |
| — LRCK | GPIO5 | 字选择 |
| — DOUT | GPIO6 | 数据输出 |
| **按键** | | 上拉输入, 按下接地 |
| — KEY_A | GPIO0 | 上 / 上一条 |
| — KEY_B | GPIO1 | 下 / 下一条 |
| — KEY_C | GPIO2 | 输入 / 发音 / 长按进入 Wi-Fi 配置 |
| — KEY_D | GPIO3 | 删除 / 模式切换 / 长按返回 |
| — KEY_E | GPIO14 | 左方向键 (Wi-Fi 配置 UI) |
| — KEY_F | GPIO15 | 右方向键 (Wi-Fi 配置 UI) |
| **SD 卡** | | SPI 模式 |
| — MOSI | GPIO11 | |
| — MISO | GPIO13 | |
| — SCLK | GPIO12 | |
| — CS | GPIO10 | |

## 软件架构

固件采用模块化设计，每个模块对应一个 Task (F-xx)，源码位于 [`src/`](src/)。

### 模块总览

| 模块 | 文件 | 职责 |
|------|------|------|
| **主入口** | [`main.c`](src/main.c) | 启动流程编排、按键路由、后台心跳/OTA 任务 |
| **日志** | [`debug_log`](src/debug_log.h) | 统一 LOG_I / LOG_W / LOG_E / LOG_D 宏封装 |
| **屏幕驱动** | [`epd_driver`](src/epd_driver.h) | EPDiy 封装: 全刷 / 局刷 / 清屏 / 深度休眠 |
| **音频** | [`audio_player`](src/audio_player.h) | I2S + MAX98357A, 44.1kHz/16bit, WAV/MP3 播放 |
| **按键** | [`button_handler`](src/button_handler.h) | 6 键轮询去抖, 区分短按 / 长按 (1.5s) |
| **存储** | [`storage_manager`](src/storage_manager.h) | SD 卡 SPI 挂载至 `/sdcard`, 文件读写 |
| **刷新调度** | [`refresh_scheduler`](src/refresh_scheduler.h) | 局刷计数, 达阈值自动全刷清残影 |
| **词库** | [`word_parser`](src/word_parser.h) | 解析 `words.json` 词库至内存 |
| **SRS 引擎** | [`srs_engine`](src/srs_engine.h) | SM-2 间隔重复算法 (纯算法, 可单测) |
| **模式状态机** | [`study_mode_machine`](src/study_mode_machine.h) | 闪卡 / 听写 / 复习三模式切换 |
| **Wi-Fi 联网** | [`wifi_manager`](src/wifi_manager.h) | STA 连接、NVS 凭据持久化、AP 扫描、断线重连 |
| **Wi-Fi 配置 UI** | [`wifi_config_ui`](src/wifi_config_ui.h) | 扫描列表 + QWERTY 软键盘配网向导 |
| **HTTP 同步** | [`sync_client`](src/sync_client.h) | 增量词库拉取、学习记录回传、心跳上报 |
| **OTA** | [`ota_manager`](src/ota_manager.h) | 双分区升级: 下载 / 校验 / 切换 / 回滚 |

### 启动流程

```
app_main()
  │
  ├─ 1. 日志 + NVS 初始化
  ├─ 2. SD 卡挂载 + 屏幕初始化 + 音频 + 按键
  ├─ 3. 刷新调度器 (局刷阈值=8)
  ├─ 4. Wi-Fi 联网 (尝试已保存凭据)
  ├─ 4.5 Wi-Fi 配置 UI 初始化 ── 无凭据时自动进入配置页
  ├─ 5. 标记固件有效 (防 OTA 回滚)
  ├─ 6. 加载词库 (/sdcard/words.json)
  ├─ 7. 进入上次学习模式
  └─ 8. 启动后台任务 (心跳 + OTA 检查, 每 10 分钟)
```

主任务退出后，系统由**按键回调**与**后台任务**事件驱动。

## 核心功能

### 学习模式

三种模式循环切换 (D 键切换)，模式持久化到 NVS:

| 模式 | 说明 |
|------|------|
| **闪卡** (FLASH) | 看词猜义，C 键发音 |
| **听写** (DICTATION) | 听音拼写 |
| **复习** (REVIEW) | SRS 到期词复习 |

### SM-2 间隔重复算法

```
回忆质量 q (0~5) → 更新 EaseFactor → 计算下次复习间隔
  q < 3:  重置间隔为 1 天 (重新学习)
  q >= 3: 1天 → 6天 → 6×EF天 → … (间隔逐步增长)
```

### 防残影刷新调度

```
局刷计数 < 8:  使用局部刷新 MODE_A2 (<1s, 快速翻页)
局刷计数 >= 8: 强制全屏刷新 MODE_GL16 (清残影) → 计数归零
```

### Wi-Fi 配置 UI

设备首次使用或需要更换网络时，通过屏幕引导完成 Wi-Fi 配网。

**进入方式：**
- 开机时无已保存凭据 → 自动进入
- 运行中长按 C 键 → 手动进入

**交互流程：**

```
列表页 (扫描附近 AP)
  ├─ A/B: 上下选择网络
  ├─ C:   确认 → 进入密码输入页
  ├─ D:   短按/长按退出配置
  │
  └─→ 密码页 (QWERTY 软键盘)
       ├─ A/B: 键盘上下移动
       ├─ E/F: 键盘左右移动
       ├─ C:   输入字符 / 触发"OK"连接
       ├─ D:   短按删除字符 / 长按返回列表
       │
       └─→ 连接 → 结果页 → 自动返回学习模式
```

**软键盘三态：** 小写字母 / 大写字母 (Shift) / 数字符号 (123)

**线程模型：** 独立 `wifi_ui_task` 处理所有 UI 逻辑 (扫描/连接/全刷)，按键事件通过 FreeRTOS 队列非阻塞转发，不影响 20ms 周期的按键去抖扫描。

### OTA 双分区升级

```
检查更新 → 下载固件 → 写入备用 OTA 分区 → MD5 校验 → 切换启动分区
                                  ↓ 校验失败
                              放弃升级，保持当前分区
启动后标记有效 → 防止异常回滚
```

## 按键操作速查

### 学习模式 (默认)

| 键 | 短按 | 长按 |
|----|------|------|
| A | 上一条 | — |
| B | 下一条 | — |
| C | 发音 | 进入 Wi-Fi 配置 |
| D | 切换学习模式 | 清残影全刷 |
| E | — | — |
| F | — | — |

### Wi-Fi 配置 UI

| 键 | 列表页 | 密码页 |
|----|--------|--------|
| A | 上移选中 | 键盘上移 |
| B | 下移选中 | 键盘下移 |
| C | 确认 / 重新扫描 | 输入字符 / 连接 |
| D | 退出 | 删除 / 长按返回列表 |
| E | — | 键盘左移 |
| F | — | 键盘右移 |

## 目录结构

```
InkWord_Firmware/
├── src/
│   ├── main.c                  # 主入口
│   ├── debug_log.{c,h}         # 日志封装
│   ├── gpio_config.h           # 全局引脚映射
│   ├── epd_driver.{c,h}        # 墨水屏驱动
│   ├── audio_player.{c,h}      # I2S 音频
│   ├── button_handler.{c,h}    # 按键扫描去抖
│   ├── storage_manager.{c,h}   # SD 卡存储
│   ├── refresh_scheduler.{c,h} # 刷新调度
│   ├── word_parser.{c,h}       # 词库解析
│   ├── srs_engine.{c,h}        # SM-2 算法
│   ├── study_mode_machine.{c,h}# 学习模式状态机
│   ├── wifi_manager.{c,h}      # Wi-Fi 联网
│   ├── wifi_config_ui.{c,h}    # Wi-Fi 配置 UI
│   ├── sync_client.{c,h}       # HTTP 同步
│   ├── ota_manager.{c,h}       # OTA 升级
│   ├── CMakeLists.txt          # ESP-IDF 组件注册
│   └── idf_component.yml       # ESP-IDF 组件依赖清单
├── partitions_default.csv      # 分区表 (OTA 双分区)
├── platformio.ini              # PlatformIO 配置
├── .gitignore
└── README.md
```

## 分区表

支持 OTA 双 app 分区方案:

| 分区 | 类型 | 偏移 | 大小 |
|------|------|------|------|
| nvs | data | 0x9000 | 24 KB |
| phy_init | data | 0xF000 | 4 KB |
| factory | app | 0x10000 | 3 MB |
| ota_0 | app | — | 3 MB |
| ota_1 | app | — | 3 MB |
| otadata | data | — | 8 KB |
| nvs_keys | data | — | 4 KB |
| spiffs | data | — | 1 MB |

## 构建与烧录

### 环境要求

- PlatformIO Core (通过 `pip install platformio`)
- ESP-IDF v5.x (PlatformIO 自动管理)
- Python 3.8+

### 编译与上传

```bash
cd InkWord_Firmware

# 编译
pio run

# 编译并烧录 (通过 USB)
pio run -t upload

# 串口监控 (115200 baud, 自带异常解码 + 颜色)
pio device monitor

# 编译 + 烧录 + 监控 一条命令
pio run -t upload && pio device monitor
```

### 依赖组件

| 依赖 | 来源 | 用途 |
|------|------|------|
| `vroland/epdiy @ ^7.0.0` | PlatformIO Library Registry | 墨水屏驱动 |
| `esp_wifi` | ESP-IDF 内置 | Wi-Fi STA |
| `esp_https_ota` | ESP-IDF 内置 | OTA 升级 |
| `fatfs` / `spiffs` | ESP-IDF 内置 | 文件系统 |
| `json` (cJSON) | ESP-IDF 内置 | 词库解析 |

### SD 卡词库格式

将 `words.json` 放置于 SD 卡根目录，格式示例:

```json
[
  {
    "text": "abandon",
    "phonetic": "/əˈbændən/",
    "meaning": "vt. 放弃; 抛弃",
    "example": "He abandoned his car.",
    "audio": "abandon.mp3",
    "tag": "CET4",
    "difficulty": 3,
    "id": 1
  }
]
```

音频文件放置于 `/sdcard/audio/` 目录。

## 配置

关键构建参数定义在 [`platformio.ini`](platformio.ini):

```ini
-D CONFIG_IDF_TARGET_ESP32S3=1           # 目标芯片
-D CONFIG_EPD_BOARD_V7_V3=1              # EPDiy 板载版本
-D CONFIG_EPD_DISPLAY_TYPE_ED097TC2=1    # 屏幕型号
```

日志级别可通过 `menuconfig` → Component config → Log output 调整，或运行时调用 `log_set_global_level()`。

## License

Proprietary — LexInk
