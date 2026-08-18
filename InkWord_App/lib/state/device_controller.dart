/// 设备连接状态（ChangeNotifier，跨页面共享）
///
/// 管理：当前连接主机（IP / inkword.local）、最近一次状态、
/// 最近成功设备持久化（shared_preferences）。
library;

import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../core/epd_protocol.dart';
import '../services/http_device_client.dart';

class DeviceController extends ChangeNotifier {
  static const _prefHost = 'last_device_host';
  static const _prefLabel = 'last_device_label';

  DeviceHttpClient? _client;
  String? _host;
  String? _label;
  WifiStatus? _lastStatus;
  bool _connecting = false;
  String? _lastError;

  DeviceHttpClient? get client => _client;
  String? get host => _host;
  String? get label => _label;
  WifiStatus? get lastStatus => _lastStatus;
  bool get connecting => _connecting;
  String? get lastError => _lastError;
  bool get connected => _client != null;

  /// 启动时恢复上次设备（异步，不阻塞 UI）
  Future<void> restoreLastDevice() async {
    final prefs = await SharedPreferences.getInstance();
    final host = prefs.getString(_prefHost);
    if (host == null || host.isEmpty) return;
    // 后台静默探测；失败不打扰（用户可重新发现）
    final client = DeviceHttpClient(host);
    try {
      final st = await client.fetchStatus();
      _adopt(client, host, prefs.getString(_prefLabel) ?? host, st);
    } catch (_) {
      client.close();
    }
  }

  /// 连接指定主机（IP 或 inkword.local），探测成功即采用
  Future<bool> connectHost(String host, {String? label}) async {
    _connecting = true;
    _lastError = null;
    notifyListeners();

    final client = DeviceHttpClient(host);
    try {
      final st = await client.fetchStatus();
      _adopt(client, host, label ?? host, st);
      _connecting = false;
      notifyListeners();
      return true;
    } catch (e) {
      client.close();
      _lastError = e.toString();
      _connecting = false;
      notifyListeners();
      return false;
    }
  }

  void _adopt(
    DeviceHttpClient client,
    String host,
    String label,
    WifiStatus st,
  ) {
    _client?.close();
    _client = client;
    _host = host;
    _label = label;
    _lastStatus = st;
    _persist(host, label);
  }

  /// 刷新设备状态（下拉/手动刷新）
  Future<void> refreshStatus() async {
    final c = _client;
    if (c == null) return;
    try {
      _lastStatus = await c.fetchStatus();
      _lastError = null;
    } catch (e) {
      _lastError = e.toString();
    }
    notifyListeners();
  }

  /// 配网成功后直接采用设备上报的 IP
  Future<void> adoptProvisionedIp(String ip) async {
    final ok = await connectHost(ip, label: 'InkWord ($ip)');
    if (ok) {
      final prefs = await SharedPreferences.getInstance();
      await prefs.setString(_prefLabel, 'InkWord ($ip)');
    }
  }

  void disconnect() {
    _client?.close();
    _client = null;
    _host = null;
    _label = null;
    _lastStatus = null;
    _lastError = null;
    notifyListeners();
  }

  /// 非致命提示（如“mDNS 查询中…”），不改变连接状态
  void connectingHint(String? hint) {
    _lastError = hint;
    notifyListeners();
  }

  Future<void> _persist(String host, String label) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_prefHost, host);
    await prefs.setString(_prefLabel, label);
  }

  /// 便捷：mDNS 主机名
  static String get mdnsHost => EpdProtocol.mdnsHost;
}
