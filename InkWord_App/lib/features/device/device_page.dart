/// 设备页：连接状态 + 发现入口（BLE 优先 / mDNS / 手动 IP / 热点兜底）
library;

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../../services/mdns_finder.dart';
import '../../state/device_controller.dart';
import '../compose/compose_page.dart';
import 'portal_fallback_page.dart';
import 'provision_wizard.dart';

class DevicePage extends StatelessWidget {
  const DevicePage({super.key});

  @override
  Widget build(BuildContext context) {
    final dev = context.watch<DeviceController>();
    return Scaffold(
      appBar: AppBar(title: const Text('InkWord 设备')),
      body: ListView(
        padding: const EdgeInsets.all(12),
        children: [
          _StatusCard(dev: dev),
          const SizedBox(height: 12),
          _ActionButton(
            icon: Icons.bluetooth,
            title: 'BLE 发现与配网',
            subtitle: '推荐：扫描设备（广播含 IP）、配网 Wi-Fi（需固件支持 BLE）',
            onTap: () => Navigator.of(
              context,
            ).push(MaterialPageRoute(builder: (_) => const ProvisionWizard())),
          ),
          _ActionButton(
            icon: Icons.search,
            title: 'mDNS 查找（inkword.local）',
            subtitle: '与设备同一 Wi-Fi 时可尝试；Android 上成功率一般',
            busy: dev.connecting,
            onTap: () => _connectMdns(context, dev),
          ),
          _ActionButton(
            icon: Icons.keyboard,
            title: '手动输入设备 IP',
            subtitle: '设备屏幕“LAN Receive”页会显示 IP（长按 F 键进入）',
            onTap: () => _inputIp(context, dev),
          ),
          _ActionButton(
            icon: Icons.wifi_tethering,
            title: '热点配网指引（兜底）',
            subtitle: '连 InkWord-Setup 热点，网页配网（无需 BLE）',
            onTap: () => Navigator.of(context).push(
              MaterialPageRoute(builder: (_) => const PortalFallbackPage()),
            ),
          ),
          const SizedBox(height: 16),
          if (dev.connected)
            FilledButton.icon(
              onPressed: () => Navigator.of(
                context,
              ).push(MaterialPageRoute(builder: (_) => const ComposePage())),
              icon: const Icon(Icons.send),
              label: const Text('去发送内容'),
            ),
        ],
      ),
    );
  }

  Future<void> _connectMdns(BuildContext context, DeviceController dev) async {
    final messenger = ScaffoldMessenger.of(context);
    dev.connectingHint('mDNS 查询中…');
    final ip = await resolveInkwordHost();
    if (ip == null) {
      dev.connectingHint(null);
      messenger.showSnackBar(
        const SnackBar(
          content: Text('mDNS 未找到设备（Android 常见）：请用 BLE 扫描或手动输入 IP'),
          duration: Duration(seconds: 4),
        ),
      );
      return;
    }
    final ok = await dev.connectHost(ip, label: 'InkWord ($ip)');
    if (!ok && context.mounted) {
      messenger.showSnackBar(
        SnackBar(
          content: Text('找到 $ip 但连接失败：${dev.lastError ?? "未知错误"}'),
          duration: const Duration(seconds: 4),
        ),
      );
    }
  }

  Future<void> _inputIp(BuildContext context, DeviceController dev) async {
    final ctrl = TextEditingController();
    final messenger = ScaffoldMessenger.of(context);
    final ip = await showDialog<String>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('输入设备 IP'),
        content: TextField(
          controller: ctrl,
          keyboardType: TextInputType.number,
          decoration: const InputDecoration(hintText: '如 192.168.1.23'),
          autofocus: true,
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(ctx),
            child: const Text('取消'),
          ),
          FilledButton(
            onPressed: () => Navigator.pop(ctx, ctrl.text.trim()),
            child: const Text('连接'),
          ),
        ],
      ),
    );
    if (ip == null || ip.isEmpty) return;
    final ok = await dev.connectHost(ip, label: 'InkWord ($ip)');
    if (!ok) {
      messenger.showSnackBar(
        SnackBar(
          content: Text(
            '连接失败：${dev.lastError ?? "未知错误"}'
            '（iOS 首次需允许“本地网络”权限）',
          ),
          duration: const Duration(seconds: 4),
        ),
      );
    }
  }
}

class _StatusCard extends StatelessWidget {
  final DeviceController dev;
  const _StatusCard({required this.dev});

  @override
  Widget build(BuildContext context) {
    final st = dev.lastStatus;
    final stateLabel = switch (st?.state) {
      'ok' => '设备在线',
      'connecting' => 'Wi-Fi 连接中',
      'fail' => 'Wi-Fi 连接失败',
      _ => '未知',
    };
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(14),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Icon(
                  dev.connected ? Icons.link : Icons.link_off,
                  color: dev.connected ? Colors.green : Colors.grey,
                ),
                const SizedBox(width: 8),
                Expanded(
                  child: Text(
                    dev.connected ? (dev.label ?? dev.host ?? '已连接') : '未连接设备',
                    style: const TextStyle(
                      fontSize: 16,
                      fontWeight: FontWeight.bold,
                    ),
                  ),
                ),
                if (dev.connected)
                  IconButton(
                    icon: const Icon(Icons.refresh),
                    tooltip: '刷新状态',
                    onPressed: dev.refreshStatus,
                  ),
              ],
            ),
            if (dev.connected) ...[
              const SizedBox(height: 4),
              Text(
                '${dev.host} · $stateLabel${st?.ip != null ? " · IP ${st!.ip}" : ""}',
                style: const TextStyle(fontSize: 13, color: Colors.black54),
              ),
            ],
            if (dev.lastError != null) ...[
              const SizedBox(height: 4),
              Text(
                dev.lastError!,
                style: const TextStyle(fontSize: 12, color: Colors.red),
              ),
            ],
          ],
        ),
      ),
    );
  }
}

class _ActionButton extends StatelessWidget {
  final IconData icon;
  final String title;
  final String subtitle;
  final VoidCallback? onTap;
  final bool busy;

  const _ActionButton({
    required this.icon,
    required this.title,
    required this.subtitle,
    this.onTap,
    this.busy = false,
  });

  @override
  Widget build(BuildContext context) {
    return Card(
      margin: const EdgeInsets.only(bottom: 8),
      child: ListTile(
        leading: Icon(icon, color: Theme.of(context).colorScheme.primary),
        title: Text(title),
        subtitle: Text(subtitle, style: const TextStyle(fontSize: 12)),
        trailing: busy
            ? const SizedBox(
                width: 18,
                height: 18,
                child: CircularProgressIndicator(strokeWidth: 2),
              )
            : const Icon(Icons.chevron_right),
        onTap: busy ? null : onTap,
      ),
    );
  }
}
