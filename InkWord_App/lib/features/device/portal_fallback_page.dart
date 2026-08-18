/// 热点配网兜底指引（无 BLE 时的传统路径）
///
/// 固件 Captive Portal：SoftAP InkWord-Setup + DNS 劫持 + 302 重定向，
/// 手机连热点后自动弹出配网页（也可手动访问 192.168.4.1）。
library;

import 'package:flutter/material.dart';

import '../../core/epd_protocol.dart';

class PortalFallbackPage extends StatelessWidget {
  const PortalFallbackPage({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('热点配网指引')),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: const [
          Text(
            '适用于：手机无 BLE，或设备固件未启用 BLE 服务。',
            style: TextStyle(color: Colors.black54),
          ),
          SizedBox(height: 16),
          _StepTile(
            n: 1,
            title: '让设备进入配网模式',
            body:
                '设备无保存的 Wi-Fi 时开机自动进入；'
                '或长按 F 键进入接收页后按提示操作。',
          ),
          _StepTile(
            n: 2,
            title: '手机连接热点 ${EpdProtocol.portalSsid}',
            body: '开放网络，无需密码。',
          ),
          _StepTile(
            n: 3,
            title: '自动弹出配网页',
            body: '连接后系统会自动弹出；若未弹出，用浏览器打开 ${EpdProtocol.portalUrl}',
          ),
          _StepTile(
            n: 4,
            title: '选择网络并输入密码',
            body:
                '页面会扫描附近 Wi-Fi，选择 SSID 输入密码后连接；'
                '连接成功后设备自动关闭热点并恢复学习界面。',
          ),
          _StepTile(
            n: 5,
            title: '回到本 App',
            body: '手机重连回家里 Wi-Fi 后，用 BLE 扫描 / 手动 IP 连接设备即可发送内容。',
          ),
        ],
      ),
    );
  }
}

class _StepTile extends StatelessWidget {
  final int n;
  final String title;
  final String body;

  const _StepTile({required this.n, required this.title, required this.body});

  @override
  Widget build(BuildContext context) {
    return Card(
      margin: const EdgeInsets.only(bottom: 10),
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            CircleAvatar(
              radius: 12,
              child: Text('$n', style: const TextStyle(fontSize: 12)),
            ),
            const SizedBox(width: 10),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    title,
                    style: const TextStyle(fontWeight: FontWeight.bold),
                  ),
                  const SizedBox(height: 4),
                  Text(body, style: const TextStyle(fontSize: 13)),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }
}
