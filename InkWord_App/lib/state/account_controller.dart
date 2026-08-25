/// 轻账户状态（v1.5 T5.3，ChangeNotifier 跨页共享）
///
/// 管理：云端 baseUrl（家长可配，默认本机开发实例）、learner token /
/// 账户名（shared_preferences 持久化）、CloudClient 生命周期。启动
/// restore 时以 /api/account/me 静默校验 token（过期即登出，不打扰）。
/// 离线优先：本控制器全部失败不影响设备直连功能。
library;

import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../services/cloud_client.dart';

class AccountController extends ChangeNotifier {
  static const _prefBase = 'cloud_base_url';
  static const _prefToken = 'cloud_token';
  static const _prefName = 'cloud_username';
  static const _prefDisplay = 'cloud_display_name';
  static const defaultBaseUrl = 'http://localhost:5228';

  CloudClient? _client;
  String _baseUrl = defaultBaseUrl;
  String? _token;
  String? _username;
  String? _displayName;
  bool _restoring = true;

  CloudClient? get client => _client;
  String get baseUrl => _baseUrl;
  String? get username => _username;
  String? get displayName => _displayName;
  bool get loggedIn => _token != null && _client != null;
  bool get restoring => _restoring;

  /// 启动恢复（后台静默；token 过期自动清理）
  Future<void> restore() async {
    final prefs = await SharedPreferences.getInstance();
    _baseUrl = prefs.getString(_prefBase) ?? defaultBaseUrl;
    final token = prefs.getString(_prefToken);
    if (token != null && token.isNotEmpty) {
      final c = CloudClient(baseUrl: _baseUrl, token: token);
      try {
        final me = await c.me();
        _client = c;
        _token = token;
        _username = me['username'] ?? prefs.getString(_prefName);
        _displayName = me['displayName'] ?? prefs.getString(_prefDisplay);
      } catch (_) {
        c.close();
        await _clearPrefs(prefs);
      }
    }
    _restoring = false;
    notifyListeners();
  }

  /// 切换云端地址（未登录也可配；已登录则需重新登录）
  Future<void> setBaseUrl(String url) async {
    final trimmed = url.trim();
    if (trimmed.isEmpty) return;
    _baseUrl = trimmed;
    _client?.close();
    _client = null;
    _token = null;
    _username = null;
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_prefBase, trimmed);
    await _clearPrefs(prefs);
    notifyListeners();
  }

  Future<void> login(String username, String password) async {
    final c = CloudClient(baseUrl: _baseUrl);
    final r = await c.login(username, password);
    await _adopt(c, r);
  }

  Future<void> register(
    String username,
    String password,
    String? displayName,
  ) async {
    final c = CloudClient(baseUrl: _baseUrl);
    final r = await c.register(username, password, displayName);
    await _adopt(c, r);
  }

  Future<void> _adopt(CloudClient c, Map<String, String> r) async {
    _client?.close();
    _client = c;
    _token = r['token'];
    _username = r['username'];
    _displayName = r['displayName'];
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_prefToken, _token!);
    await prefs.setString(_prefName, _username ?? '');
    await prefs.setString(_prefDisplay, _displayName ?? '');
    notifyListeners();
  }

  Future<void> logout() async {
    _client?.close();
    _client = null;
    _token = null;
    _username = null;
    _displayName = null;
    await _clearPrefs(await SharedPreferences.getInstance());
    notifyListeners();
  }

  Future<void> _clearPrefs(SharedPreferences prefs) async {
    await prefs.remove(_prefToken);
    await prefs.remove(_prefName);
    await prefs.remove(_prefDisplay);
  }
}
