/// 冒烟测试：App 能构建主页面
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:inkword_app/main.dart';
import 'package:inkword_app/features/device/device_page.dart';

void main() {
  testWidgets('App 启动渲染设备页', (WidgetTester tester) async {
    await tester.pumpWidget(const InkWordApp());
    expect(find.byType(DevicePage), findsOneWidget);
    expect(find.text('BLE 发现与配网'), findsWidgets);
  });
}
