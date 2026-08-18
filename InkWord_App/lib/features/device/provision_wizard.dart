/// BLE 配网向导：扫描设备 → 连接 → 选网输密 → 状态跟踪 → 采纳 IP
library;

import 'dart:async';

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../../services/ble_service.dart';
import '../../services/http_device_client.dart';
import '../../state/device_controller.dart';

class ProvisionWizard extends StatefulWidget {
  const ProvisionWizard({super.key});

  @override
  State<ProvisionWizard> createState() => _ProvisionWizardState();
}

enum _Stage { scan, provisioning, done }

class _ProvisionWizardState extends State<ProvisionWizard> {
  _Stage _stage = _Stage.scan;

  // 扫描段
  bool _scanning = false;
  List<BleDeviceSnapshot> _devices = [];
  StreamSubscription<List<BleDeviceSnapshot>>? _scanSub;
  String? _scanHint;

  // 配网段
  BleProvisionConnection? _conn;
  BleDeviceSnapshot? _device;
  StreamSubscription<WifiStatus>? _statusSub;
  WifiStatus? _status;
  bool _connectingBle = false;
  String? _error;

  final _ssidCtrl = TextEditingController();
  final _passCtrl = TextEditingController();
  List<WifiAp> _aps = [];
  bool _scanningWifi = false;
  bool _sending = false;

  // 完成段
  String? _provisionedIp;

  @override
  void initState() {
    super.initState();
    _startScan();
  }

  @override
  void dispose() {
    _scanSub?.cancel();
    _statusSub?.cancel();
    _conn?.disconnect();
    _ssidCtrl.dispose();
    _passCtrl.dispose();
    super.dispose();
  }

  Future<void> _startScan() async {
    setState(() {
      _scanning = true;
      _scanHint = null;
    });
    try {
      final ok = await BleService.ensurePermissions();
      if (!ok) {
        setState(() => _scanHint = '未授予蓝牙权限，请到系统设置开启');
      }
      final adapterOn = await BleService.isAdapterOn();
      if (!adapterOn) {
        setState(() => _scanHint = '蓝牙未开启，请先打开系统蓝牙');
      }
      await _scanSub?.cancel();
      _scanSub = BleService.scanResultsStream.listen((list) {
        if (mounted) setState(() => _devices = list);
      });
      await BleService.startScan();
    } catch (e) {
      setState(() => _scanHint = '扫描失败：$e');
    } finally {
      if (mounted) setState(() => _scanning = false);
    }
  }

  Future<void> _connectDevice(BleDeviceSnapshot d) async {
    setState(() {
      _connectingBle = true;
      _device = d;
      _error = null;
    });
    try {
      final conn = await BleService.connect(d.remoteId);
      _statusSub?.cancel();
      _statusSub = conn.statusStream.listen(_onStatus);
      if (mounted) {
        setState(() {
          _conn = conn;
          _stage = _Stage.provisioning;
        });
      }
    } catch (e) {
      if (mounted) setState(() => _error = '$e');
    } finally {
      if (mounted) setState(() => _connectingBle = false);
    }
  }

  void _onStatus(WifiStatus st) {
    if (!mounted) return;
    setState(() => _status = st);
    if (st.state == 'ok' && st.ip != null && st.ip!.isNotEmpty) {
      setState(() {
        _provisionedIp = st.ip;
        _stage = _Stage.done;
      });
    } else if (st.state == 'error') {
      setState(
        () => _error = switch (st.reason) {
          'device-config-ui' => '设备正在软键盘配网界面，请在设备上退出后重试',
          'connecting' => '设备正在连接中，请等待结果',
          'bad-json' => '数据格式错误，请重试',
          'invalid' => 'SSID 或密码过长，请检查',
          _ => '设备拒绝：${st.reason ?? "未知原因"}',
        },
      );
    } else if (st.state == 'fail') {
      setState(() => _error = '连接失败，请检查密码后重试');
    }
  }

  Future<void> _scanWifi() async {
    final conn = _conn;
    if (conn == null) return;
    setState(() {
      _scanningWifi = true;
      _aps = [];
      _error = null;
    });
    try {
      final aps = await conn.scanWifi();
      if (mounted) setState(() => _aps = aps);
    } catch (e) {
      if (mounted) setState(() => _error = '$e');
    } finally {
      if (mounted) setState(() => _scanningWifi = false);
    }
  }

  Future<void> _sendCredentials() async {
    final conn = _conn;
    final ssid = _ssidCtrl.text.trim();
    if (conn == null || ssid.isEmpty) {
      setState(() => _error = '请选择或输入 SSID');
      return;
    }
    setState(() {
      _sending = true;
      _error = null;
    });
    try {
      await conn.sendCredentials(ssid, _passCtrl.text);
      // 结果经 statusStream 推送（connecting → ok/fail/error）
    } catch (e) {
      setState(() => _error = '发送失败：$e（设备可能已断开）');
    } finally {
      if (mounted) setState(() => _sending = false);
    }
  }

  Future<void> _finish() async {
    final ip = _provisionedIp;
    if (ip != null) {
      await context.read<DeviceController>().adoptProvisionedIp(ip);
    }
    if (mounted) {
      Navigator.of(context).pop(); // 回设备页（已连接，可去发送）
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('BLE 发现与配网')),
      body: switch (_stage) {
        _Stage.scan => _buildScanStage(),
        _Stage.provisioning => _buildProvisionStage(),
        _Stage.done => _buildDoneStage(),
      },
    );
  }

  Widget _buildScanStage() => Column(
    children: [
      Padding(
        padding: const EdgeInsets.all(12),
        child: Row(
          children: [
            Expanded(
              child: Text(
                _scanHint ?? '正在扫描附近 InkWord 设备…（广播携带 Wi-Fi 状态与 IP）',
                style: TextStyle(
                  color: _scanHint == null ? Colors.black54 : Colors.red,
                  fontSize: 13,
                ),
              ),
            ),
            TextButton(
              onPressed: _scanning ? null : _startScan,
              child: Text(_scanning ? '扫描中…' : '重新扫描'),
            ),
          ],
        ),
      ),
      if (_connectingBle) const LinearProgressIndicator(),
      Expanded(
        child: _devices.isEmpty
            ? const Center(
                child: Text('未发现设备\n请确认墨水屏已开机', textAlign: TextAlign.center),
              )
            : ListView.builder(
                itemCount: _devices.length,
                itemBuilder: (_, i) {
                  final d = _devices[i];
                  return ListTile(
                    leading: const Icon(Icons.contactless),
                    title: Text(d.name),
                    subtitle: Text(
                      '${d.stateLabel}${d.ip != null ? ' · ${d.ip}' : ' · 无 IP'} · ${d.rssi} dBm',
                    ),
                    trailing: d.wifiState == 2
                        ? const Chip(label: Text('可直接使用'))
                        : const Chip(label: Text('需配网')),
                    onTap: () => _connectDevice(d),
                  );
                },
              ),
      ),
      if (_error != null)
        Padding(
          padding: const EdgeInsets.all(12),
          child: Text(_error!, style: const TextStyle(color: Colors.red)),
        ),
    ],
  );

  Widget _buildProvisionStage() => ListView(
    padding: const EdgeInsets.all(12),
    children: [
      Card(
        child: Padding(
          padding: const EdgeInsets.all(12),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(
                '已连接：${_device?.name ?? ""}',
                style: const TextStyle(fontWeight: FontWeight.bold),
              ),
              const SizedBox(height: 4),
              Text(
                '设备状态：${_statusStateLabel(_status?.state)}'
                '${_status?.ip != null ? " · IP ${_status!.ip}" : ""}',
              ),
            ],
          ),
        ),
      ),
      const SizedBox(height: 8),
      Row(
        children: [
          Expanded(
            child: OutlinedButton.icon(
              onPressed: _scanningWifi ? null : _scanWifi,
              icon: const Icon(Icons.wifi_find),
              label: Text(_scanningWifi ? '扫描中（1~2 秒）…' : '扫描 Wi-Fi（设备侧）'),
            ),
          ),
        ],
      ),
      if (_aps.isNotEmpty)
        ConstrainedBox(
          constraints: const BoxConstraints(maxHeight: 220),
          child: ListView.builder(
            shrinkWrap: true,
            itemCount: _aps.length,
            itemBuilder: (_, i) {
              final ap = _aps[i];
              return ListTile(
                dense: true,
                title: Text(ap.ssid),
                subtitle: Text('${ap.rssi} dBm${ap.auth ? " · 加密" : ""}'),
                trailing: _ssidCtrl.text == ap.ssid
                    ? const Icon(Icons.check)
                    : null,
                onTap: () => setState(() => _ssidCtrl.text = ap.ssid),
              );
            },
          ),
        ),
      TextField(
        controller: _ssidCtrl,
        decoration: const InputDecoration(labelText: 'SSID（可手动输入）'),
        onChanged: (_) => setState(() {}),
      ),
      TextField(
        controller: _passCtrl,
        obscureText: true,
        decoration: const InputDecoration(labelText: '密码'),
      ),
      const SizedBox(height: 12),
      FilledButton.icon(
        onPressed: _sending ? null : _sendCredentials,
        icon: const Icon(Icons.send),
        label: Text(_sending ? '发送中…' : '发送配网'),
      ),
      const SizedBox(height: 8),
      if (_status?.state == 'connecting')
        const Row(
          children: [
            SizedBox(
              width: 16,
              height: 16,
              child: CircularProgressIndicator(strokeWidth: 2),
            ),
            SizedBox(width: 8),
            Text('设备连接 Wi-Fi 中…'),
          ],
        ),
      if (_error != null)
        Padding(
          padding: const EdgeInsets.only(top: 8),
          child: Text(_error!, style: const TextStyle(color: Colors.red)),
        ),
      TextButton(
        onPressed: () async {
          await _conn?.disconnect();
          if (mounted) {
            setState(() {
              _stage = _Stage.scan;
              _conn = null;
              _status = null;
              _aps = [];
            });
            _startScan();
          }
        },
        child: const Text('返回重选设备'),
      ),
    ],
  );

  Widget _buildDoneStage() => Center(
    child: Column(
      mainAxisAlignment: MainAxisAlignment.center,
      children: [
        const Icon(Icons.check_circle, color: Colors.green, size: 64),
        const SizedBox(height: 12),
        Text('配网成功！设备 IP：$_provisionedIp'),
        const SizedBox(height: 24),
        FilledButton(onPressed: _finish, child: const Text('完成并连接设备')),
      ],
    ),
  );

  String _statusStateLabel(String? s) => switch (s) {
    'connecting' => '连接中',
    'ok' => '已联网',
    'fail' => '连接失败',
    'error' => '拒绝请求',
    _ => '未配网/空闲',
  };
}
