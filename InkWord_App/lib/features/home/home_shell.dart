/// 底部导航壳（v1.3 T3.4）：设备 / 词书 / 报告 三主页面
///
/// IndexedStack 保活各页状态（切换 tab 不丢连接与列表）；
/// 设备页保持原 AppBar，词书/报告页各自管理标题
/// （P2 起统计页升级为「学习报告」：本地统计 + 云端报告）。
library;

import 'package:flutter/material.dart';

import '../deck/deck_page.dart';
import '../device/device_page.dart';
import '../stats/stats_page.dart';

class HomeShell extends StatefulWidget {
  const HomeShell({super.key});

  @override
  State<HomeShell> createState() => _HomeShellState();
}

class _HomeShellState extends State<HomeShell> {
  int _tab = 0;

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: IndexedStack(
        index: _tab,
        children: const [DevicePage(), DeckPage(), StatsPage()],
      ),
      bottomNavigationBar: NavigationBar(
        selectedIndex: _tab,
        onDestinationSelected: (i) => setState(() => _tab = i),
        destinations: const [
          NavigationDestination(icon: Icon(Icons.devices), label: '设备'),
          NavigationDestination(icon: Icon(Icons.menu_book), label: '词书'),
          NavigationDestination(icon: Icon(Icons.insights), label: '报告'),
        ],
      ),
    );
  }
}
