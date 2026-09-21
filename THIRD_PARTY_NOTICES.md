# 第三方组件与内容来源声明

本仓库包含第三方组件与内容，权利归各自权利人所有。清单如下，版本以各工程锁文件为准。

## 一、固件（GPL-3.0 侧）

| 组件 | 许可 | 说明 |
|:--|:--|:--|
| [GxEPD2](https://github.com/ZinggJM/GxEPD2) 1.6.9 | **GPL-3.0** | 墨水屏驱动库；本项目固件 GPL-3.0 义务的主要来源 |
| `InkWord_Firmware/src/GxEPD2_374_DEPG0370.{h,cpp}`<br>`InkWord_Firmware/src/GxEPD2_gdeq031t10.{h,cpp}`<br>`InkWord_Firmware/src/probe/probe_gxepd2_uc8151d.cpp` | **GPL-3.0** | 由 GxEPD2 派生 / 改编，按 GPL-3.0 分发 |
| [Adafruit GFX Library](https://github.com/adafruit/Adafruit-GFX-Library) | BSD | 图形基元 |
| [Adafruit BusIO](https://github.com/adafruit/Adafruit_BusIO) | BSD | I²C / SPI 封装 |
| [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) | Apache-2.0 | BLE 协议栈（含 Apache NimBLE） |
| ESP-IDF 组件 | Apache-2.0（部分 BSD / MIT） | Espressif |
| Arduino-ESP32 core | LGPL-2.1-or-later | 可升级至 LGPL-3.0，与 GPL-3.0 兼容 |
| [Unity](https://github.com/ThrowTheSwitch/Unity) | MIT | 单元测试框架（仅测试构建） |
| [cJSON](https://github.com/DaveGamble/cJSON) | MIT | JSON 解析（测试） |

分发固件二进制时须一并提供本声明及对应源码获取方式（GPL-3.0 第 6 条义务）。

## 二、内容资产（不随代码许可授权）

| 资产 | 来源 | 许可 / 义务 |
|:--|:--|:--|
| 英语词库（中考 / 高考 / 人教版分册） | qwerty-learner 词库数据 | **GPL-3.0**；派生的 `default_words.csv` / `default_words.json` 按 GPL-3.0 分发 |
| 初中、高考古诗文 | tangyuan0821/Junior-Middle-School-poetry<br>clover-yan/gaokao-poetry | **CC-BY-SA-4.0**：须署名 + 相同方式共享；派生的 `poems_cb.csv` 及选目编排同受约束 |
| 小学必背古诗词 | MIZHANG08/AncientPoemsPrimary | 上游未标注许可 —— **商用前须确认授权** |
| 《传习录》引文（`InkWord_Firmware/tools/chuanxilu_quotes.txt`） | 王阳明（明代） | 文本已进入公有领域 |
| 点阵字库（`cjk_font_data.bin`、`cjk_font.c/h` 等） | 由 `InkWord_Firmware/tools/gen_cjk_font.*` 渲染字体生成 | **发行构建须使用 `--ofl` 模式**（仅可自由再分发字体：Noto / 思源 / DejaVu），以规避 Apple 等系统字体的嵌入许可限制 |
| 词库读音音频 | `tools/default_vocab/fetch_audio.py` 本地生成 | **不随仓库分发**（`.gitignore` 排除）；商用分发前须确认音源授权 |

> 内容资产的商业使用（付费卡组、教材同步、音频下发等）不在代码开源许可范围内，
> 详见 [`LICENSE`](LICENSE)「不在代码许可范围内的资产」。

## 三、App / Admin / 后端

依赖均为 MIT / BSD / Apache-2.0 等宽松许可，完整清单与版本见各工程锁文件：
`InkWord_App/pubspec.lock`、`InkWord_Admin/package-lock.json`、`InkWord_Backend/src/**/*.csproj`。
