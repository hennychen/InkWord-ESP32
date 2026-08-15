# InkWord Firmware

基于 ESP32-S3 的墨水屏英语单词学习终端固件，运行于 Arduino 框架 (espressif32@7.0.1) + GxEPD2 驱动栈。

## 硬件方案（2026-08 实测版）

| 组件 | 型号 / 规格 | 接口 |
|------|-------------|------|
| 主控 | ESP32-S3-DevKitC-1 (16MB Flash, 8MB PSRAM) | — |
| 屏幕 | DKE DEPG0370 3.7" 240×416 (UC8253 类 COG) + EVK011 升压转接板 | 4 线 SPI 直驱 (GxEPD2) |
| 音频 | MAX98357A 功放 | I2S（待接线验证） |
| 存储 | MicroSD 卡 | SPI + FAT（未接线） |
| 按键 | 6 个轻触按键 (A~F) | GPIO 独立输入（待接线验证） |

> 详细接线图见 [`../docs/WIRING_DIAGRAM.md`](../docs/WIRING_DIAGRAM.md)。
> 升压：EVK011 板上分立 boost 由屏 COG 从 FPC pin2(GDR) 自主驱动，
> MCU 仅向 J2-16 供 3.3V，无 GDR/RESE 信号。

## 引脚映射

完整定义见 [`src/gpio_config.h`](src/gpio_config.h)。

| 外设 | 引脚 | 说明 |
|------|------|------|
| **墨水屏** (EVK011 J2, 9 线) | | 4 线 SPI, BS=GPIO11 固件驱动 LOW (绝不可悬空) |
| — SCK | GPIO7 → J2-3 | SPI 时钟 |
| — SDO | GPIO8 → J2-5 | SPI 数据 (MCU 侧 MOSI，转接板丝印 SDO) |
| — D/C# | GPIO9 → J2-7 | 命令/数据 |
| — CS | GPIO10 → J2-6 | 片选 |
| — BS | GPIO11 → J2-10 | 接口模式选择, 固件驱动 LOW=4 线 SPI (备选方案: 板侧短接 GND + `EPD_BS_PIN=-1`) |
| — RST | GPIO13 → J2-8 | 硬复位 (深睡唯一唤醒途径) |
| — BUSY | GPIO12 → J2-9 | 忙信号 (LOW=忙) |
| — VCI/GND | 3V3 → J2-16 / GND → J2-1 | 供电与共地 (必须!) |
| **I2S 音频** | | 标准飞利浦 I2S |
| — BCLK | GPIO4 | 位时钟 |
| — LRCK | GPIO5 | 字选择 |
| — DOUT | GPIO6 | 数据输出 |
| **按键** | | 上拉输入, 按下接地 |
| — KEY_A | GPIO0 | 上 / 上一条 |
| — KEY_B | GPIO1 | 下 / 下一条 |
| — KEY_C | GPIO2 | 输入 / 发音 / 长按进入 Wi-Fi 配置 |
| — KEY_D | GPIO3 | 删除 / 模式切换 / 长按清残影全刷 |
| — KEY_E | GPIO14 | 左方向键 (Wi-Fi 配置 UI) |
| — KEY_F | GPIO15 | 右方向键 (Wi-Fi 配置 UI) |
| **SD 卡** (SPI3_HOST) | | 独立于 EPD 的 SPI 总线 |
| — MOSI | GPIO17 | |
| — MISO | GPIO16 | |
| — SCLK | GPIO18 | |
| — CS | GPIO47 | |

> ⚠️ SD 卡引脚勿与墨水屏混淆：GPIO10/11/12/13 全部为 EPD 占用
> (CS/BS/BUSY/RST)，SD 必须用 GPIO16/17/18/47。

## 软件架构

固件采用模块化设计，每个模块对应一个 Task (F-xx)，源码位于 [`src/`](src/)。

### 模块总览

| 模块 | 文件 | 职责 |
|------|------|------|
| **主入口** | [`main.cpp`](src/main.cpp) | 启动流程编排、按键路由、单词卡片 UI 渲染（局刷/全刷策略）、后台心跳/OTA任务 |
| **日志** | [`debug_log`](src/debug_log.h) | 统一 LOG_I / LOG_W / LOG_E / LOG_D 宏封装 |
| **屏幕驱动** | [`epd_driver`](src/epd_driver.h) + [`GxEPD2_374_DEPG0370`](src/GxEPD2_374_DEPG0370.h) | GxEPD2 自有面板类: 全刷/局刷/清屏/深睡; C 薄适配层 epd_gfx_*; 双坐标体系（底层竖屏 240×416 直通 / GFX 层横屏 416×240） |
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
setup() (Arduino)
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
局刷计数 < 8:  GxEPD2 displayWindow 差分局刷 (仅内容区, ~350ms, 无闪烁)
局刷计数 >= 8: 强制全屏刷新清残影 → 计数归零
GFX 局刷路径 (epd_gfx_flush_window): 先调 refresh_gfx_before_partial()
前置检查，返回 true 时需整屏重绘后全刷
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
│   ├── main.cpp                # 主入口 (Arduino setup/loop)
│   ├── debug_log.{c,h}         # 日志封装
│   ├── gpio_config.h           # 全局引脚映射
│   ├── GxEPD2_374_DEPG0370.{cpp,h} # GxEPD2 面板类 (DEPG0370 专用初始化序列)
│   ├── Fonts/Arial14pt7b.h    # 字体 (fontconvert 生成)
│   ├── epd_driver.{cpp,h}      # 墨水屏驱动适配层 (epd_gfx_* C API)
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

- PlatformIO Core（macOS 需 Homebrew Python 3.11+）
- espressif32@7.0.1 平台包（PlatformIO 自动管理，Arduino 框架）

### 编译与上传

```bash
cd InkWord_Firmware

# 编译（必须用 Python 3.11，系统默认 pio 跑在旧 Python 上会报版本错）
/opt/homebrew/bin/python3.11 -m platformio run -e inkword-s3

# 编译并烧录 (USB, CP2102 通常为 /dev/cu.usbserial-0001)
/opt/homebrew/bin/python3.11 -m platformio run -e inkword-s3 -t upload

# 串口监控 (115200; 烧录前需先退出，否则串口占用报 port is busy)
/opt/homebrew/bin/python3.11 -m platformio device monitor --port /dev/cu.usbserial-0001 --baud 115200
```

> 注：Arduino 预编译库 CONFIG_LOG_MAXIMUM_LEVEL=ERROR，串口只能看到
> E 级日志与 Serial.printf 输出，LOG_I/LOG_W 在编译期被裁剪。

### 依赖组件

| 依赖 | 来源 | 用途 |
|------|------|------|
| `GxEPD2 @ 1.6.9` | PlatformIO Library Registry | 墨水屏驱动基类 |
| `Adafruit GFX` @ 1.12.6 | PlatformIO Library Registry | 绘图栈/字体渲染 |
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

关键配置位于 [`platformio.ini`](platformio.ini)（板型 esp32-s3-devkitc-1、
环境 inkword-s3）与 [`sdkconfig.inkword-s3`](sdkconfig.inkword-s3)。
屏幕初始化序列在 [`GxEPD2_374_DEPG0370.cpp`](src/GxEPD2_374_DEPG0370.cpp)
（PSR=0xDF、全刷 CDI=0x97、局刷 CDI=0x17，对照官方 demo 修正）。

日志级别可通过 `menuconfig` → Component config → Log output 调整，或运行时调用 `log_set_global_level()`。

## License

Proprietary — LexInk
