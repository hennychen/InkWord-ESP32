/// wrapTextLines 断行规则单测
///
/// 语义基准 = 固件网页端 PAGE_HTML renderText 的折行循环
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:inkword_app/services/render/frame_painter.dart';

/// 假测宽器：每字符 10px（便于手算）
double m(String s) => s.length * 10.0;

void main() {
  test('超宽换行：新行从溢出字符开始，空格保留在行内', () {
    // "AAAA BBBB"，maxW=45：AAAA(40)+" "→50 溢出；" BBB"(40)+"B"→50 再溢出
    final lines = wrapTextLines(
      sourceLines: ['AAAA BBBB'],
      maxW: 45,
      lh: 31.2,
      vh: 416,
      measure: m,
    );
    expect(lines, ['AAAA', ' BBB', 'B']);
  });

  test('未超宽不换行', () {
    final lines = wrapTextLines(
      sourceLines: ['ABC DEF'],
      maxW: 240 - 16,
      lh: 31.2,
      vh: 416,
      measure: m,
    );
    expect(lines, ['ABC DEF']);
  });

  test('换行符分段：每段独立折行，空段保留为空行', () {
    final lines = wrapTextLines(
      sourceLines: ['AB', '', 'CD'],
      maxW: 224,
      lh: 31.2,
      vh: 416,
      measure: m,
    );
    expect(lines, ['AB', '', 'CD']);
  });

  test('固定宽度两字符折行（不丢字符）', () {
    final lines = wrapTextLines(
      sourceLines: ['ABCDEF'],
      maxW: 25, // 任何两字符(20)≤25，三字符(30)>25
      lh: 31.2,
      vh: 416,
      measure: m,
    );
    expect(lines, ['AB', 'CD', 'EF']);
  });

  test('视口高度截断到 floor(vh/lh) 行', () {
    // lh=31.2, vh=240（横屏视口）→ maxLines=7
    final src = List.generate(20, (i) => 'L$i');
    final lines = wrapTextLines(
      sourceLines: src,
      maxW: 400,
      lh: 31.2,
      vh: 240,
      measure: m,
    );
    expect(lines.length, 7);
    expect(lines.first, 'L0');
  });

  test('CJK 逐字折行', () {
    final lines = wrapTextLines(
      sourceLines: ['你好墨水屏'],
      maxW: 35, // 3 字(30)≤35，4 字(40)>35
      lh: 31.2,
      vh: 416,
      measure: m,
    );
    expect(lines, ['你好墨', '水屏']);
  });
}
