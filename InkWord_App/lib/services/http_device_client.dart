/// 设备 HTTP 客户端：对接固件 lan_display_server.cpp 的局域网 API
///
/// - POST /api/display      12480 字节裸帧（application/octet-stream）
/// - GET  /api/wifi/status  {"state":"idle|connecting|ok|fail","ip":"..."}
/// - GET  /api/wifi/scan    [{"ssid":"...","rssi":-60,"auth":true}]
/// - POST /api/wifi/connect {"ssid":"...","pass":"..."}
library;

import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:http/http.dart' as http;

import '../../core/epd_protocol.dart';

/// 设备不可达（不在同一网络 / IP 错误 / 本地网络权限被拒）
class DeviceUnreachableException implements Exception {
  final String detail;
  DeviceUnreachableException(this.detail);
  @override
  String toString() => '设备不可达: $detail';
}

/// 设备忙（配网 UI 激活等，对应固件 503）
class DeviceBusyException implements Exception {
  @override
  String toString() => '设备忙（配网界面占用中），请稍后重试';
}

/// 设备返回的其他错误（4xx/5xx）
class DeviceHttpException implements Exception {
  final int statusCode;
  final String body;
  DeviceHttpException(this.statusCode, this.body);
  @override
  String toString() => '设备错误 $statusCode: $body';
}

/// Wi-Fi 连接状态（镜像固件 wconn_state_t + HTTP status 语义）
class WifiStatus {
  /// idle | connecting | ok | fail | error
  final String state;
  final String? ip;

  /// state==error 时的原因（BLE 配网拒绝：device-config-ui / connecting / bad-json / invalid）
  final String? reason;
  const WifiStatus(this.state, this.ip, {this.reason});

  bool get isOk => state == 'ok';

  factory WifiStatus.fromJson(Map<String, dynamic> j) => WifiStatus(
    j['state'] as String? ?? 'idle',
    j['ip'] as String?,
    reason: j['reason'] as String?,
  );
}

/// 扫描到的 Wi-Fi 热点
class WifiAp {
  final String ssid;
  final int rssi;
  final bool auth;
  const WifiAp(this.ssid, this.rssi, this.auth);

  factory WifiAp.fromJson(Map<String, dynamic> j) => WifiAp(
    j['ssid'] as String? ?? '',
    (j['rssi'] as num?)?.toInt() ?? 0,
    j['auth'] == true,
  );
}

class DeviceHttpClient {
  /// 设备主机（IP 或 inkword.local）
  final String host;

  /// 连通性探测超时（mDNS/手动输入后首次验证）
  static const probeTimeout = Duration(seconds: 3);

  /// 整帧发送超时（12KB 上传 + 设备端全刷耗时）
  static const sendTimeout = Duration(seconds: 10);

  final http.Client _http;

  DeviceHttpClient(this.host, {http.Client? client})
    : _http = client ?? http.Client();

  Uri _u(String path) => Uri.parse('http://$host$path');

  void close() => _http.close();

  Exception _mapError(Object e) {
    if (e is SocketException || e is http.ClientException) {
      return DeviceUnreachableException(e.toString());
    }
    if (e is TimeoutException) {
      return DeviceUnreachableException('请求超时');
    }
    return e is Exception ? e : Exception(e.toString());
  }

  void _checkStatus(http.Response resp) {
    if (resp.statusCode == 503) throw DeviceBusyException();
    if (resp.statusCode != 200) {
      throw DeviceHttpException(resp.statusCode, resp.body);
    }
  }

  /// 探测设备是否可用（GET /api/wifi/status）
  Future<WifiStatus> fetchStatus({Duration timeout = probeTimeout}) async {
    try {
      final resp = await _http
          .get(_u(EpdProtocol.pathWifiStatus))
          .timeout(timeout);
      _checkStatus(resp);
      return WifiStatus.fromJson(_decodeJson(resp.body));
    } catch (e) {
      throw _mapError(e);
    }
  }

  /// 发送整帧（12480 字节 1bpp，固件校验长度后整帧直刷）
  Future<void> postDisplay(
    Uint8List frame, {
    Duration timeout = sendTimeout,
  }) async {
    if (frame.length != EpdProtocol.frameBytes) {
      throw ArgumentError(
        'frame must be ${EpdProtocol.frameBytes} bytes, got ${frame.length}',
      );
    }
    try {
      final resp = await _http
          .post(
            _u(EpdProtocol.pathDisplay),
            headers: {'Content-Type': 'application/octet-stream'},
            body: frame,
          )
          .timeout(timeout);
      _checkStatus(resp);
    } catch (e) {
      if (e is DeviceBusyException || e is DeviceHttpException) rethrow;
      throw _mapError(e);
    }
  }

  /// 设备侧 Wi-Fi 扫描（阻塞 1~2 秒）
  Future<List<WifiAp>> fetchWifiScan() async {
    try {
      final resp = await _http
          .get(_u(EpdProtocol.pathWifiScan))
          .timeout(const Duration(seconds: 8));
      _checkStatus(resp);
      final list = _decodeJson(resp.body) as List;
      return list
          .map((e) => WifiAp.fromJson((e as Map).cast<String, dynamic>()))
          .toList();
    } catch (e) {
      if (e is DeviceBusyException || e is DeviceHttpException) rethrow;
      throw _mapError(e);
    }
  }

  /// 请求设备异步连接 Wi-Fi（结果经 [fetchStatus] 轮询）
  Future<void> postWifiConnect(String ssid, String pass) async {
    try {
      final resp = await _http
          .post(
            _u(EpdProtocol.pathWifiConnect),
            headers: {'Content-Type': 'application/json'},
            body: jsonEncode({'ssid': ssid, 'pass': pass}),
          )
          .timeout(const Duration(seconds: 5));
      _checkStatus(resp);
    } catch (e) {
      if (e is DeviceBusyException || e is DeviceHttpException) rethrow;
      throw _mapError(e);
    }
  }

  dynamic _decodeJson(String body) => jsonDecode(body);
}
