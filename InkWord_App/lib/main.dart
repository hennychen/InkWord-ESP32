/// InkWord App 入口
///
/// 纯局域网直连架构：BLE 发现/配网 + mDNS/手动 IP 兜底 + HTTP 传图传文本。
library;

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import 'features/device/device_page.dart';
import 'state/device_controller.dart';

void main() {
  runApp(const InkWordApp());
}

class InkWordApp extends StatelessWidget {
  const InkWordApp({super.key});

  @override
  Widget build(BuildContext context) {
    return ChangeNotifierProvider(
      create: (_) => DeviceController()..restoreLastDevice(),
      child: MaterialApp(
        title: 'InkWord',
        debugShowCheckedModeBanner: false,
        theme: ThemeData(
          colorSchemeSeed: const Color(0xFF2E5077),
          useMaterial3: true,
          brightness: Brightness.light,
        ),
        home: const DevicePage(),
      ),
    );
  }
}
