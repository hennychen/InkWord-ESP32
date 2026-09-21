# 贡献指南

## 可以贡献什么

- **固件**（`InkWord_Firmware/`、`InkWord_Firmware_BigScreen/`）：面板适配、驱动、UI、SRS 引擎、页面迁移
- **后端**（`InkWord_Backend/`）、**App**（`InkWord_App/`）、**Admin**（`InkWord_Admin/`）
- **工具链**（`tools/`）与**设计文档**（`docs/`）
- **内容勘误**：词条释义/音标错误可按条提交修正；大段词库数据请勿直接提交（见「不接受的内容」）

## 开发环境

| 组件 | 环境 | 编译 / 测试 |
|:--|:--|:--|
| 固件 | PlatformIO + **Python 3.11** | `python3.11 -m platformio run -e inkword-s3`<br>`python3.11 -m platformio test -e native-test` |
| 后端 | .NET 8 | `dotnet test` |
| App | Flutter | `flutter test` |
| Admin | Angular 17 | `npm test` |

macOS 下 `python3.11` 通常为 `/opt/homebrew/bin/python3.11`；多屏构建矩阵见
[`docs/PANEL_COMPAT_DESIGN.md`](docs/PANEL_COMPAT_DESIGN.md)。

## 提交前检查（门禁）

1. **编译零新增警告**——本项目执行零警告纪律，框架级噪音已在 `platformio.ini` 过滤；
2. **`native-test` 全绿**（`pio test -e native-test`）；
3. **固件行为变更附证据**：串口日志或真机效果，涉及屏显的变更建议附黄金帧对比；
4. **文档与代码同步**：引脚、接线、协议、菜单结构变更必须同步更新 `docs/` 与 README 对应章节；
5. **勿手改生成物**：`cjk_font_data.bin`、`cjk_font.c/h`、`default_words.json` 等由脚本生成，
   改脚本后重新运行（见 `InkWord_Firmware/tools/` 文件头用法）。

## 提交信息格式

```
type(scope): 中文摘要

- 变更点一（中文 bullet points）
- 变更点二
```

`type` 取 `feat` / `fix` / `docs` / `chore` / `refactor` / `test`；正文使用中文。

## 许可与贡献者协议（必读）

本项目采用**分层许可**（固件 GPL-3.0 / 后端 AGPL-3.0 / App、Admin、工具、文档 Apache-2.0，
详见 [`LICENSE`](LICENSE)）。提交贡献前请确认以下三条：

1. **DCO 签署**：使用 `git commit -s` 提交，声明你有权提交该贡献、来源合法；
2. **接受 CLA**：你同意 [`CLA.md`](CLA.md) 中的贡献者许可协议——**你保留版权**，
   同时授予项目方永久、全球、免费、不可撤销、可再许可（含商业授权）的权利。
   这是项目维持「开源 + 商业授权」双轨所必需；不接受该条款的贡献无法合并；
3. **第三方材料披露**：贡献中含第三方代码、数据或资源时，须在 PR 中标注来源与许可。

## 不接受的内容

- **生成产物**（应由脚本重新生成，不接收手改版本）；
- **未获授权的内容数据**：词库、读音音频、字库、教材内容等（授权规则见
  [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)）；
- **品牌使用**：使用项目名称/标识的方式须符合 [`TRADEMARK.md`](TRADEMARK.md)；
- 密钥、凭据、个人本地路径、与功能无关的格式化改动。
