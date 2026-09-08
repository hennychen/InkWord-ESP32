# inkword_app

InkWord 家长端 App（Flutter）：设备配网/绑定、词书管理、卡组编辑器、学习报告。
对接后端 `api/me/*`（账户 JWT）与设备 LAN 直连端点。

## 功能模块

| 页面 | 功能 |
|:--|:--|
| 设备 | LAN 配网 / 我的设备绑定与解绑（登录态） |
| 词书 | 词书列表切换 / 上传 / 今日统计 |
| 报告 | LAN 本地统计保留 + 云端学习报告（登录态）：设备选择器 / 对话周报（五段解析·历史周切换）/ 学习聚合（墨封数跨设备去重）/ 阅读进度 / 云端书库只读浏览（下载引导走设备端「我的书架→云端书架」） |
| 编辑器 | 卡组编辑器：登录注册 / 我的卡组与本地草稿 / 条目 CRUD·CSV 导入·LAN 推送 / 分享与发现 |

> 云端报告依赖设备同步上报与周日 ChatReview Job；未同步/无周报时对应卡片空态提示。

## Getting Started

This project is a starting point for a Flutter application.

A few resources to get you started if this is your first Flutter project:

- [Lab: Write your first Flutter app](https://docs.flutter.dev/get-started/codelab)
- [Cookbook: Useful Flutter samples](https://docs.flutter.dev/cookbook)

For help getting started with Flutter development, view the
[online documentation](https://docs.flutter.dev/), which offers tutorials,
samples, guidance on mobile development, and a full API reference.
