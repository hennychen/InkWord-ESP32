/// BLE 配网服务（flutter_blue_plus 2.x）
///
/// 职责（仅发现/配网/状态，不传输内容）：
/// - 扫描：按 manufacturer data（厂商 ID 0x02E5 + 协议版本）过滤 InkWord 设备，
///   广播载荷携带 Wi-Fi 状态与设备 IP（发现闭环主通道，mDNS 为兜底）
/// - 连接：GATT 服务 cc5a0001-...；creds(write) / status(read+notify) / scan(write+notify)
///
/// 协议常量见 core/epd_protocol.dart（与固件 ble_provision.cpp 双侧同步）。
library;

import 'dart:async';
import 'dart:convert';
import 'dart:io' show Platform;
import 'dart:typed_data';

import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';

import '../core/epd_protocol.dart';
import 'http_device_client.dart' show WifiAp, WifiStatus;

class BleProvisionException implements Exception {
  final String msg;
  BleProvisionException(this.msg);
  @override
  String toString() => msg;
}

/// 扫描到的设备快照（来自广播 manufacturer data）
class BleDeviceSnapshot {
  final String remoteId;
  final String name;
  final int wifiState; // 0 idle / 1 connecting / 2 ok / 3 fail
  final String? ip;
  final int rssi;
  const BleDeviceSnapshot({
    required this.remoteId,
    required this.name,
    required this.wifiState,
    this.ip,
    required this.rssi,
  });

  String get stateLabel => switch (wifiState) {
    1 => '连接中',
    2 => '已联网',
    3 => '连接失败',
    _ => '未配网',
  };
}

/// 已建立的配网连接（用完 [disconnect]）
class BleProvisionConnection {
  final BluetoothDevice device;
  final BluetoothCharacteristic credsChar;
  final BluetoothCharacteristic statusChar;
  final BluetoothCharacteristic scanChar;

  BleProvisionConnection(
    this.device,
    this.credsChar,
    this.statusChar,
    this.scanChar,
  );

  /// 状态流：先推一次当前读值，再转发 notify
  Stream<WifiStatus> get statusStream async* {
    yield _parseStatus(await statusChar.read());
    yield* statusChar.onValueReceived.map(_parseStatus);
  }

  /// 写入 Wi-Fi 凭据；结果经 [statusStream] 观察
  /// （接受 → connecting；拒绝 → {"state":"error","reason":...}）
  Future<void> sendCredentials(String ssid, String pass) async {
    final payload = utf8.encode(jsonEncode({'ssid': ssid, 'pass': pass}));
    await credsChar.write(payload);
  }

  /// 触发设备侧 Wi-Fi 扫描并收集 notify 推送直至 end 标记
  Future<List<WifiAp>> scanWifi({
    Duration timeout = const Duration(seconds: 12),
  }) async {
    final completer = Completer<void>();
    final aps = <WifiAp>[];
    var err = 'timeout';
    late final StreamSubscription<List<int>> sub;
    sub = scanChar.onValueReceived.listen((v) {
      try {
        final j = jsonDecode(utf8.decode(v)) as Map<String, dynamic>;
        if (j['end'] == true) {
          err = (j['err'] as String?) ?? '';
          if (!completer.isCompleted) completer.complete();
        } else {
          aps.add(WifiAp.fromJson(j));
        }
      } catch (_) {
        // 忽略畸形包
      }
    });
    try {
      await scanChar.write([0x01]);
      await completer.future.timeout(timeout, onTimeout: () {});
    } finally {
      await sub.cancel();
    }
    if (err.isNotEmpty) {
      throw BleProvisionException('设备扫描失败（$err），请稍后重试');
    }
    return aps;
  }

  Future<void> disconnect() async {
    try {
      await device.disconnect();
    } catch (_) {
      // 断开失败忽略（可能已断）
    }
  }
}

/// BLE 扫描/连接门面
class BleService {
  BleService._();

  /// 请求 BLE 相关权限。
  ///
  /// Android 12+：BLUETOOTH_SCAN/CONNECT；Android ≤11 回退请求定位权限；
  /// iOS：使用期系统自动弹蓝牙权限（NSBluetoothAlwaysUsageDescription）。
  static Future<bool> ensurePermissions() async {
    if (!Platform.isAndroid) return true;
    final statuses = await [
      Permission.bluetoothScan,
      Permission.bluetoothConnect,
    ].request();
    var ok = statuses.values.every(
      (s) => s.isGranted || s.isLimited || s.isProvisional,
    );
    if (!ok) {
      // 旧机型（API≤30）无新蓝牙权限，扫描依赖定位权限
      final loc = await Permission.locationWhenInUse.request();
      ok = loc.isGranted;
    }
    return ok;
  }

  /// 蓝牙适配器是否已开启（未开启时引导用户去系统设置）
  static Future<bool> isAdapterOn() async {
    try {
      return await FlutterBluePlus.adapterState
          .where(
            (s) =>
                s != BluetoothAdapterState.unknown &&
                s != BluetoothAdapterState.turningOn,
          )
          .map((s) => s == BluetoothAdapterState.on)
          .first
          .timeout(const Duration(seconds: 3));
    } catch (_) {
      return false;
    }
  }

  /// 开始扫描（manufacturer data 过滤，10 秒自动停止）
  static Future<void> startScan({
    Duration timeout = const Duration(seconds: 10),
  }) async {
    await FlutterBluePlus.startScan(
      withMsd: [MsdFilter(EpdProtocol.bleMsdCompanyId)],
      timeout: timeout,
    );
  }

  static Future<void> stopScan() => FlutterBluePlus.stopScan();

  /// 扫描结果流（已过滤并解析 manufacturer data；无效广播被丢弃）
  static Stream<List<BleDeviceSnapshot>> get scanResultsStream =>
      FlutterBluePlus.scanResults.map(
        (results) => [
          for (final r in results)
            if (_parseAdvertisement(r) != null) _parseAdvertisement(r)!,
        ],
      );

  static BleDeviceSnapshot? _parseAdvertisement(ScanResult r) {
    final md =
        r.advertisementData.manufacturerData[EpdProtocol.bleMsdCompanyId];
    if (md == null || md.length < EpdProtocol.bleMsdPayloadLen) return null;
    if (md[0] != EpdProtocol.bleProtoVersion) return null;
    final wifiState = md[1];
    final hasIp = md[2] != 0 || md[3] != 0 || md[4] != 0 || md[5] != 0;
    final ip = hasIp ? '${md[2]}.${md[3]}.${md[4]}.${md[5]}' : null;
    return BleDeviceSnapshot(
      remoteId: r.device.remoteId.str,
      name: r.advertisementData.advName.isNotEmpty
          ? r.advertisementData.advName
          : r.device.remoteId.str,
      wifiState: wifiState,
      ip: ip,
      rssi: r.rssi,
    );
  }

  /// 连接设备并发现服务（Android 上 connect 默认协商 MTU 512）
  static Future<BleProvisionConnection> connect(String remoteId) async {
    final device = BluetoothDevice.fromId(remoteId);
    try {
      await device.connect(
        license: License.nonprofit, // 个人/非营利用途许可
        timeout: const Duration(seconds: 8),
        mtu: 512,
      );
    } catch (e) {
      throw BleProvisionException('连接设备失败：$e');
    }

    try {
      final services = await device.discoverServices();
      final svc = services.firstWhere(
        (s) => s.serviceUuid.str == EpdProtocol.bleServiceUuid,
        orElse: () => throw BleProvisionException('设备未提供配网服务（固件版本过旧？）'),
      );
      BluetoothCharacteristic byUuid(String uuid) {
        return svc.characteristics.firstWhere(
          (c) => c.characteristicUuid.str == uuid,
          orElse: () => throw BleProvisionException('特征缺失：$uuid（固件版本过旧？）'),
        );
      }

      final conn = BleProvisionConnection(
        device,
        byUuid(EpdProtocol.bleCharCredsUuid),
        byUuid(EpdProtocol.bleCharStatusUuid),
        byUuid(EpdProtocol.bleCharScanUuid),
      );
      await conn.statusChar.setNotifyValue(true);
      await conn.scanChar.setNotifyValue(true);
      return conn;
    } catch (e) {
      await device.disconnect();
      if (e is BleProvisionException) rethrow;
      throw BleProvisionException('服务发现失败：$e');
    }
  }
}

WifiStatus _parseStatus(List<int> raw) {
  try {
    final j =
        jsonDecode(utf8.decode(Uint8List.fromList(raw)))
            as Map<String, dynamic>;
    return WifiStatus.fromJson(j);
  } catch (_) {
    return const WifiStatus('idle', null);
  }
}
