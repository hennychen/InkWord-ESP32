/// InkWord App 入口
///
/// 局域网直连架构：BLE 发现/配网 + mDNS/手动 IP 兜底 + HTTP 传图传文本；
/// v1.5 T5.3 增轻账户（云端卡组编辑器，离线优先——不登录不影响直连）。
library;

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import 'features/home/home_shell.dart';
import 'services/cloud_client.dart';
import 'state/account_controller.dart';
import 'state/device_controller.dart';

void main() {
  runApp(const InkWordApp());
}

class InkWordApp extends StatelessWidget {
  const InkWordApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MultiProvider(
      providers: [
        ChangeNotifierProvider(
          create: (_) => DeviceController()..restoreLastDevice(),
        ),
        // v1.5 T5.3 轻账户：登录态 + 云客户端派生（未登录 = null）
        ChangeNotifierProvider(create: (_) => AccountController()..restore()),
        ProxyProvider<AccountController, CloudClient?>(
          update: (_, account, __) => account.client,
        ),
      ],
      child: MaterialApp(
        title: 'InkWord',
        debugShowCheckedModeBanner: false,
        theme: ThemeData(
          colorSchemeSeed: const Color(0xFF2E5077),
          useMaterial3: true,
          brightness: Brightness.light,
        ),
        home: const HomeShell() /* v1.3 T3.4：设备/词书/统计三主页面 */,
      ),
    );
  }
}
