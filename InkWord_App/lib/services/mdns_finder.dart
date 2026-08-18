/// mDNS 设备发现兜底（multicast_dns）
///
/// 查询固件注册的 inkword.local A 记录（_http._tcp）。
/// Android 上 mDNS 解析常不可靠：失败时调用方应降级到手动输入 IP
/// 或 BLE 广播发现（manufacturer data 携带 IP，为主通道）。
library;

import 'dart:async';

import 'package:multicast_dns/multicast_dns.dart';

import '../core/epd_protocol.dart';

/// 解析 inkword.local → IPv4 字符串；失败/超时返回 null
Future<String?> resolveInkwordHost({
  Duration timeout = const Duration(seconds: 4),
}) async {
  final client = MDnsClient();
  await client.start();
  final completer = Completer<String?>();
  StreamSubscription<IPAddressResourceRecord>? sub;
  Timer? guard;
  try {
    sub = client
        .lookup<IPAddressResourceRecord>(
          ResourceRecordQuery.addressIPv4(EpdProtocol.mdnsHost),
          timeout: timeout,
        )
        .listen(
          (r) {
            if (!completer.isCompleted) {
              completer.complete(r.address.address);
            }
          },
          onError: (_) {
            if (!completer.isCompleted) completer.complete(null);
          },
          onDone: () {
            if (!completer.isCompleted) completer.complete(null);
          },
        );
    // 双保险：lookup 超时关流仍不触发 onDone 的极端情况
    guard = Timer(timeout * 2, () {
      if (!completer.isCompleted) completer.complete(null);
    });
    return await completer.future;
  } finally {
    guard?.cancel();
    await sub?.cancel();
    client.stop();
  }
}
