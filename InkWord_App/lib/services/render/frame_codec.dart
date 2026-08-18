/// RGBA 帧像素 → EPD 240x416 1bpp 整帧打包（纯函数，无 Flutter 绑定）
///
/// 算法与固件网页端 lan_display_server.cpp 内嵌 PAGE_HTML 的 pack() 语义一致：
/// - 灰度：ITU-R BT.601（0.299R + 0.587G + 0.114B）
/// - Floyd-Steinberg 抖动（可选）：阈值 128，误差扩散
///   右 7/16、左下 3/16、下 5/16、右下 1/16
/// - 不抖动：硬阈值 gray >= 128 → 白
/// - 打包：行宽 30 字节，MSB first，bit=1 白（0=黑）
library;

import 'dart:typed_data';

import '../../core/epd_protocol.dart';

/// [compute]/[Isolate.run] 的入口参数（顶层可发送对象）
class EncodeArgs {
  final Uint8List rgba; // 240*416*4 字节，行主序
  final bool dither;
  final bool invert;

  const EncodeArgs(this.rgba, this.dither, this.invert);
}

/// 把 240x416 RGBA 像素打包成 12480 字节 EPD 整帧。
///
/// 必须在后台 isolate 调用（[compute]），避免大循环阻塞 UI。
Uint8List encodeFrameBytes(EncodeArgs args) {
  final rgba = args.rgba;
  final w = EpdProtocol.frameWidth;
  final h = EpdProtocol.frameHeight;
  final bpr = EpdProtocol.bytesPerRow;
  final n = w * h;

  assert(rgba.length == n * 4, 'rgba must be ${n * 4} bytes');

  // 灰度化（浮点保留精度，供抖动误差扩散）
  final gray = Float32List(n);
  for (var i = 0; i < n; i++) {
    gray[i] =
        0.299 * rgba[i * 4] + 0.587 * rgba[i * 4 + 1] + 0.114 * rgba[i * 4 + 2];
  }

  final out = Uint8List(bpr * h); // 全 0 初值 = 黑

  if (args.dither) {
    for (var y = 0; y < h; y++) {
      for (var x = 0; x < w; x++) {
        final i = y * w + x;
        final old = gray[i];
        final nw = old >= 128.0 ? 255.0 : 0.0;
        var white = nw == 255.0;
        if (args.invert) white = !white;
        if (white) out[y * bpr + (x >> 3)] |= 0x80 >> (x & 7);
        final er = old - nw;
        if (x + 1 < w) gray[i + 1] += er * 7 / 16;
        if (y + 1 < h) {
          if (x > 0) gray[i + w - 1] += er * 3 / 16;
          gray[i + w] += er * 5 / 16;
          if (x + 1 < w) gray[i + w + 1] += er * 1 / 16;
        }
      }
    }
  } else {
    for (var i = 0; i < n; i++) {
      var white = gray[i] >= 128.0;
      if (args.invert) white = !white;
      if (white) {
        final x = i % w;
        final y = i ~/ w;
        out[y * bpr + (x >> 3)] |= 0x80 >> (x & 7);
      }
    }
  }

  return out;
}
