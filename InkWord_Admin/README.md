# InkWordAdmin

InkWord 管理端（Angular 17 standalone + Material）：设备/词库/卡组/书籍运营与数据看板。
对接后端 `api/admin/*` 端点（见 [docs/BACKEND_PROTOCOL.md](../docs/BACKEND_PROTOCOL.md)）。

## 模块总览

| 路由 | 模块 | 功能 |
|:--|:--|:--|
| `/dashboard` | 数据看板 | 今日学习分析 / SRS 分布 / 阅读分析区（今日活跃读者·阅读时长·热门书籍 Top10·近 7 天趋势） |
| `/wrong-top` | 错词排行 | 全设备错词聚合 |
| `/words` | 词库管理 | 词条 CRUD / CSV 导入 / 导出设备词库 / AI 生成与审核台 |
| `/decks` | 卡组管理 | 科目 chip 过滤 / 卡组列表（版式·条目数·版本·Owner·共享态）/ 条目弹窗（分页·行内编辑·新增·删除——删除为归档，已同步设备不感知需 LAN 重推覆盖）/ 字符集核对（T4.5 生僻字子集）/ AI 生成卡组入口 |
| `/books` | 书籍管理 | 书籍上传（TXT/MD/HTML ≤10MB）/ 发布开关 / 详情阅读统计（读者数·平均进度） |
| `/devices` | 设备管理 | 设备清单 / 详情（同步·学习·阅读记录区块） |
| `/ota` | OTA 升级 | 固件版本下发与灰度 |

> 写路径契约：卡组/词库条目 Version 接全局 max 递增（设备增量同步下发契约）、Front/Back 双写（T4.1）、Tag=deck.Code 隔离。

## Development server

Run `ng serve` for a dev server. Navigate to `http://localhost:4200/`. The application will automatically reload if you change any of the source files.

## Code scaffolding

Run `ng generate component component-name` to generate a new component. You can also use `ng generate directive|pipe|service|class|guard|interface|enum|module`.

## Build

Run `ng build` to build the project. The build artifacts will be stored in the `dist/` directory.

## Running unit tests

Run `ng test` to execute the unit tests via [Karma](https://karma-runner.github.io).

## Running end-to-end tests

Run `ng e2e` to execute the end-to-end tests via a platform of your choice. To use this command, you need to first add a package that implements end-to-end testing capabilities.

## Further help

To get more help on the Angular CLI use `ng help` or go check out the [Angular CLI Overview and Command Reference](https://angular.io/cli) page.
