/// frame_codec.encodeFrameBytes 黄金用例
///
/// 语义基准 = 固件网页端 lan_display_server.cpp 内嵌 PAGE_HTML 的 pack()
library;

import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:inkword_app/core/epd_protocol.dart';
import 'package:inkword_app/services/render/frame_codec.dart';

const w = EpdProtocol.frameWidth; // 240
const h = EpdProtocol.frameHeight; // 416

/// 构造全灰度 g 的 RGBA 缓冲
Uint8List allGray(int g) {
  final buf = Uint8List(w * h * 4);
  for (var i = 0; i < w * h; i++) {
    buf[i * 4] = g;
    buf[i * 4 + 1] = g;
    buf[i * 4 + 2] = g;
    buf[i * 4 + 3] = 255;
  }
  return buf;
}

/// 设置单像素灰度（其余为黑 0）
void setGray(Uint8List buf, int x, int y, int g) {
  final i = (y * w + x) * 4;
  buf[i] = g;
  buf[i + 1] = g;
  buf[i + 2] = g;
  buf[i + 3] = 255;
}

void main() {
  test('输出长度恰为 12480 字节', () {
    final out = encodeFrameBytes(EncodeArgs(_zeros(), false, false));
    expect(out.length, EpdProtocol.frameBytes);
  });

  test('全白 → 全 0xFF；全黑 → 全 0x00（不抖动）', () {
    final white = encodeFrameBytes(EncodeArgs(allGray(255), false, false));
    expect(white.every((b) => b == 0xFF), isTrue);

    final black = encodeFrameBytes(EncodeArgs(allGray(0), false, false));
    expect(black.every((b) => b == 0x00), isTrue);
  });

  test('阈值 128：gray>=128 为白（128 恰好为白）', () {
    final buf = allGray(127);
    setGray(buf, 0, 0, 128);
    final out = encodeFrameBytes(EncodeArgs(buf, false, false));
    // 仅 (0,0) 为白：首字节 0b1000_0000
    expect(out[0], 0x80);
    expect(out.sublist(1).every((b) => b == 0x00), isTrue);
  });

  test('反色：全白+反色 → 全 0x00', () {
    final out = encodeFrameBytes(EncodeArgs(allGray(255), false, true));
    expect(out.every((b) => b == 0x00), isTrue);
  });

  test('左半白右半黑（不抖动）→ 每行前 15 字节 0xFF', () {
    final buf = allGray(0);
    for (var y = 0; y < h; y++) {
      for (var x = 0; x < 120; x++) {
        setGray(buf, x, y, 255);
      }
    }
    final out = encodeFrameBytes(EncodeArgs(buf, false, false));
    for (var y = 0; y < h; y++) {
      final row = out.sublist(y * 30, (y + 1) * 30);
      expect(row.sublist(0, 15).every((b) => b == 0xFF), isTrue);
      expect(row.sublist(15).every((b) => b == 0x00), isTrue);
    }
  });

  test('Floyd-Steinberg：手算两像素用例 (200,100) → 白黑', () {
    // p0=200 → 白，er=-55 → p1=100-55*7/16=75.94 → 黑
    // 首字节应恰为 0b1000_0000=0x80；误差下行均 <128 保持黑
    final buf = allGray(0);
    setGray(buf, 0, 0, 200);
    setGray(buf, 1, 0, 100);
    final out = encodeFrameBytes(EncodeArgs(buf, true, false));
    expect(out[0], 0x80);
    expect(out.sublist(1).every((b) => b == 0x00), isTrue);
  });

  test('Floyd-Steinberg：50% 灰产生黑白混合（非纯色）', () {
    final out = encodeFrameBytes(EncodeArgs(allGray(128), true, false));
    final whiteBits = out.fold<int>(0, (acc, b) => acc + _popcount(b));
    expect(whiteBits, greaterThan(0));
    expect(whiteBits, lessThan(w * h));
  });
}

Uint8List _zeros() => Uint8List(w * h * 4);

int _popcount(int v) {
  var c = 0;
  while (v != 0) {
    c += v & 1;
    v >>= 1;
  }
  return c;
}
