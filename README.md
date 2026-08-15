# InkWord-ESP32

基于 ESP32-S3 墨水屏的英语单词学习系统 —— 全栈 IoT 解决方案，包含固件、后端服务和管理后台。

## 项目结构

```
InkWord-ESP32/
├── InkWord_Firmware/    # ESP32-S3 固件 (ESP-IDF + PlatformIO)
├── InkWord_Backend/     # 后端 API (.NET 8 + PostgreSQL + Redis)
├── InkWord_Admin/       # 管理后台 (Angular 17 + Material + ECharts)
├── docker-compose.yml   # 全栈一键编排
└── README.md
```

## 技术栈

### 固件 (`InkWord_Firmware/`)
- **芯片**: ESP32-S3 (Arduino 框架 + ESP-IDF 组件, PlatformIO espressif32@7.0.1)
- **屏幕**: DKE DEPG0370 3.7" 240x416 (UC8253 类 COG) + EVK011 升压转接板，GxEPD2 驱动栈 + Adafruit GFX
- **驱动架构**: 自有面板类 `GxEPD2_374_DEPG0370`（PSR=0xDF、全刷 CDI=0x97、局刷 CDI=0x17）+ `epd_gfx_*` C 薄适配层；双坐标体系（底层竖屏 240x416 直通 / GFX 层横屏 416x240）
- **升压**: 屏 COG 经 GDR 自主驱动 EVK011 分立 boost，MCU 仅供 VCI 3.3V（J2-16），无 GDR/RESE 信号
- **字体**: FreeSans 9/18/24pt + Arial 14pt（fontconvert 生成，默认字号）
- **音频**: I2S + MAX98357A
- **存储**: SD 卡 (SPI + FAT)
- **算法**: SM-2 间隔重复 (SRS) + 防残影刷新调度
- **网络**: WiFi STA + HTTP 同步 + OTA 双分区升级

### 后端 (`InkWord_Backend/`)
- **框架**: .NET 8 WebAPI (5 项目分层)
- **数据库**: PostgreSQL 16 + EF Core 8
- **缓存**: Redis 7 (StackExchange.Redis)
- **任务**: Hangfire (每日复习推送 / 冷词归档)
- **认证**: JWT Bearer + PBKDF2 密码哈希
- **日志**: Serilog → 文件
- **API 文档**: Swagger / OpenAPI

### 前端 (`InkWord_Admin/`)
- **框架**: Angular 17 (standalone components)
- **UI**: Angular Material (墨水屏黑白主题)
- **图表**: ECharts (ngx-echarts)
- **特性**: 暗黑模式切换 / JWT 拦截器 / 响应式表单 / Lazy-load 路由

## 快速开始

### Docker 全栈部署（推荐）

```bash
# 克隆并启动
docker compose up -d --build

# 访问
#   前端:  http://localhost
#   后端:  http://localhost:8080/swagger
#   DB:    localhost:5432
#   Redis: localhost:6379
```

### 本地开发

#### 后端
```bash
cd InkWord_Backend
dotnet restore
dotnet ef migrations add Init --project src/InkWord.Infrastructure --startup-project src/InkWord.API
dotnet run --project src/InkWord.API
# API: http://localhost:5228/swagger
```

#### 前端
```bash
cd InkWord_Admin
npm install
ng serve
# 访问: http://localhost:4200
```

#### 固件

> 需 Python 3.10+（macOS 下用 Homebrew Python 3.11；首次构建前 `pip install intelhex pyelftools`）

```bash
cd InkWord_Firmware
# 编译（必须用 Python 3.11，系统默认 pio 跑在旧 Python 上会报错）
/opt/homebrew/bin/python3.11 -m platformio run -e inkword-s3
# 编译并烧录（串口设备按实际替换，CP2102 通常为 /dev/cu.usbserial-0001）
/opt/homebrew/bin/python3.11 -m platformio run -e inkword-s3 -t upload --upload-port /dev/cu.usbserial-0001
# 串口监控（烧录前需先退出，否则串口占用报 port is busy）
/opt/homebrew/bin/python3.11 -m platformio device monitor --port /dev/cu.usbserial-0001 --baud 115200
```

硬件接线（ESP32-S3 → EVK011 J2，8 根线，2026-08 实测）：SCK=GPIO7、MOSI=GPIO8、D/C=GPIO9、CS=GPIO10、RES=GPIO13、BUSY=GPIO12、3V3→J2-16(VCI)、GND→J2-1（必须共地）。BS 线已省（板侧 J2-10 短接 GND，固件 EPD_BS_PIN=-1）。音频 I2S=GPIO4/5/6，按键 A~F=GPIO0/1/2/3/14/15，SD（未接线）=SPI3 GPIO16/17/18/47。详见 `docs/WIRING_DIAGRAM.md`。

> 已验证状态（2026-08）：屏幕点屏成功（横屏 UI + 全刷/局刷 + Wi-Fi 配置页 + BS 省线 9→8 根实测无 Busy Timeout）。SD 卡模块未接线（挂载失败不影响其他模块）；音频/按键待验证。

## API 端点概览

| 分组 | 方法 | 路径 | 说明 |
|:---|:---|:---|:---|
| **认证** | POST | `/api/auth/login` | JWT 登录 |
| **设备** | POST | `/api/device/register` | 设备注册 |
| **设备** | GET | `/api/device/sync/words` | 增量同步词库 |
| **设备** | POST | `/api/device/sync/progress` | 上传学习记录 |
| **设备** | POST | `/api/device/heartbeat` | 心跳上报 |
| **设备** | GET | `/api/device/ota/check` | 检查 OTA 更新 |
| **管理** | GET/POST/PUT/DELETE | `/api/admin/words` | 词库 CRUD |
| **管理** | POST | `/api/admin/words/import` | CSV 批量导入 |
| **管理** | GET | `/api/admin/devices` | 设备列表 |
| **管理** | POST | `/api/admin/devices/{id}/command` | 远程指令 |
| **管理** | GET | `/api/admin/dashboard/stats` | 看板统计 |

## 核心算法

### SM-2 间隔重复算法
```
质量分 q (0~5) → 更新 EaseFactor → 计算下次复习间隔
  q < 3:  重置间隔为 1 天
  q >= 3: 1天 → 6天 → 6*EF天 → ...（逐步增长）
```

### 防残影刷新调度
```
局刷计数 < 8:  使用局部刷新 (<1s)
局刷计数 >= 8: 强制全屏刷新 (清除残影) → 计数归零
GFX 局刷路径 (epd_gfx_flush_window): 调用 refresh_gfx_before_partial() 前置检查，
               返回 true 时需整屏重绘后全刷
```

## License

Proprietary — LexInk
