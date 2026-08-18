/// 内容渲染：文本/图片 → 240x416 竖屏缓冲（RGBA）→ EPD 整帧
///
/// 排版语义对齐固件网页端 lan_display_server.cpp 内嵌 PAGE_HTML 的 JS：
/// - renderText：逐字符累积折行（maxW=视口宽-16）、行高 1.3x字号、
///   首行基线 y = -vh/2 + fs + (vh-n*lh)/2（垂直居中）、超行截断
/// - drawImg：等比缩放 min(aw/w, ah/h) 居中
/// - auto 旋转：图片宽>高 → 90°（用满 416 长边）；文本 auto → 0°
/// - devView：设备横屏视角 = 竖屏缓冲逆时针转 90°
///
/// 渲染（TextPainter/Canvas 仅限主 isolate）→ RGBA →
/// [encodeFrameBytes]（后台 isolate）→ 12480 字节 1bpp 帧。
library;

import 'dart:math' as math;
import 'dart:ui' as ui;

import 'package:flutter/foundation.dart';
import 'package:flutter/painting.dart';

import '../../core/epd_protocol.dart';
import 'frame_codec.dart';

enum ContentType { text, image }

/// 旋转选项（auto/0/90/180/270，与网页端下拉一致）
enum Rotation { auto, r0, r90, r180, r270 }

/// 一次发送的内容规格（不可变，参数变化时重建）
class ContentSpec {
  final ContentType type;
  final String text;

  /// 文本字号（16/24/32/48）
  final double fontSize;

  /// 图片内容（已解码，主 isolate 使用）
  final ui.Image? image;

  /// 图片原始宽高（auto 旋转判断用，图片未加载时为 0）
  final int imageWidth;
  final int imageHeight;

  final Rotation rotation;

  const ContentSpec({
    this.type = ContentType.text,
    this.text = '',
    this.fontSize = 24,
    this.image,
    this.imageWidth = 0,
    this.imageHeight = 0,
    this.rotation = Rotation.auto,
  });
}

/// 解析实际旋转角：auto=图片横图转 90°、文本 0°；其余按指定值
int resolveRotation(ContentSpec spec) {
  switch (spec.rotation) {
    case Rotation.auto:
      if (spec.type == ContentType.image &&
          spec.imageWidth > spec.imageHeight) {
        return 90;
      }
      return 0;
    case Rotation.r0:
      return 0;
    case Rotation.r90:
      return 90;
    case Rotation.r180:
      return 180;
    case Rotation.r270:
      return 270;
  }
}

/// 逐字符折行（对齐 JS renderText 的断行循环，CJK 友好）。
///
/// [measure] 注入文本测宽函数便于单测；语义：
/// - 候选行超宽则换行，新行从当前字符开始
/// - (已出行数+1)*行高 > 视口高时停止收集
/// - 最终截断到 floor(vh/lh) 行
@visibleForTesting
List<String> wrapTextLines({
  required List<String> sourceLines,
  required double maxW,
  required double lh,
  required double vh,
  required double Function(String) measure,
}) {
  final out = <String>[];
  var stop = false;
  for (final src in sourceLines) {
    var line = '';
    for (final rune in src.runes) {
      final c = String.fromCharCode(rune);
      final t = line + c;
      if (measure(t) > maxW && line.isNotEmpty) {
        out.add(line);
        line = c;
        if ((out.length + 1) * lh > vh) {
          stop = true;
          break;
        }
      } else {
        line = t;
      }
    }
    out.add(line);
    if (stop || (out.length + 1) * lh > vh) break;
  }
  final maxLines = (vh / lh).floor();
  if (out.length > maxLines) out.length = maxLines;
  return out;
}

/// 在原点 (0,0) 的 240x416 画布上绘制内容（白底）。
///
/// 预览组件：先 canvas.scale 再调用本函数即可任意缩放；
/// 编码路径：按原始尺寸光栅化后送 [encodeFrameBytes]。
void paintContent(Canvas canvas, ContentSpec spec) {
  final w = EpdProtocol.frameWidth.toDouble();
  final h = EpdProtocol.frameHeight.toDouble();
  canvas.drawRect(
    Rect.fromLTWH(0, 0, w, h),
    Paint()..color = const Color(0xFFFFFFFF),
  );

  final rot = resolveRotation(spec);
  if (spec.type == ContentType.text) {
    _paintText(canvas, spec, rot);
  } else {
    _paintImage(canvas, spec, rot);
  }
}

/* ---------------- 文本 ---------------- */

void _paintText(Canvas canvas, ContentSpec spec, int rot) {
  final fs = spec.fontSize;
  final vw = (rot == 90 || rot == 270)
      ? EpdProtocol.frameHeight.toDouble()
      : EpdProtocol.frameWidth.toDouble();
  final vh = (rot == 90 || rot == 270)
      ? EpdProtocol.frameWidth.toDouble()
      : EpdProtocol.frameHeight.toDouble();
  final lh = fs * 1.3;
  final maxW = vw - 16;
  final style = TextStyle(fontSize: fs, color: const Color(0xFF000000));

  final lines = wrapTextLines(
    sourceLines: spec.text.split('\n'),
    maxW: maxW,
    lh: lh,
    vh: vh,
    measure: (s) => _measureText(s, style),
  );
  if (lines.isEmpty) return;

  canvas.save();
  canvas.translate(EpdProtocol.frameWidth / 2, EpdProtocol.frameHeight / 2);
  canvas.rotate(rot * math.pi / 180);

  // 首行基线 = -vh/2 + fs + (vh - n*lh)/2（对齐 JS），用实际字体 ascent 定位顶边
  var baseline = -vh / 2 + fs + (vh - lines.length * lh) / 2;
  for (final line in lines) {
    if (line.isNotEmpty) {
      final tp = TextPainter(
        text: TextSpan(text: line, style: style),
        textDirection: TextDirection.ltr,
      )..layout(maxWidth: double.infinity);
      tp.paint(canvas, Offset(-vw / 2 + 8, baseline - _firstAscent(tp)));
      tp.dispose();
    }
    baseline += lh;
  }
  canvas.restore();
}

double _measureText(String s, TextStyle style) {
  if (s.isEmpty) return 0;
  final tp = TextPainter(
    text: TextSpan(text: s, style: style),
    textDirection: TextDirection.ltr,
  )..layout();
  final w = tp.width;
  tp.dispose();
  return w;
}

double _firstAscent(TextPainter tp) {
  final metrics = tp.computeLineMetrics();
  return metrics.isNotEmpty ? metrics.first.ascent : 0;
}

/* ---------------- 图片 ---------------- */

void _paintImage(Canvas canvas, ContentSpec spec, int rot) {
  final image = spec.image;
  if (image == null) return;
  final aw = (rot == 90 || rot == 270)
      ? EpdProtocol.frameHeight.toDouble()
      : EpdProtocol.frameWidth.toDouble();
  final ah = (rot == 90 || rot == 270)
      ? EpdProtocol.frameWidth.toDouble()
      : EpdProtocol.frameHeight.toDouble();
  final s = math.min(aw / image.width, ah / image.height);
  final dw = image.width * s;
  final dh = image.height * s;

  canvas.save();
  canvas.translate(EpdProtocol.frameWidth / 2, EpdProtocol.frameHeight / 2);
  canvas.rotate(rot * math.pi / 180);
  final paint = Paint()
    ..filterQuality = FilterQuality.medium
    ..isAntiAlias = true;
  canvas.drawImageRect(
    image,
    Rect.fromLTWH(0, 0, image.width.toDouble(), image.height.toDouble()),
    Rect.fromLTWH(-dw / 2, -dh / 2, dw, dh),
    paint,
  );
  canvas.restore();
}

/* ---------------- 编码全流程 ---------------- */

/// 内容规格 → 12480 字节 EPD 整帧。
///
/// 主 isolate 光栅化 240x416 RGBA（文本/矢量绘制必须在主 isolate），
/// 灰度/抖动/打包在 [compute] 后台 isolate 完成。
Future<Uint8List> renderSpecToFrame(
  ContentSpec spec, {
  required bool dither,
  required bool invert,
}) async {
  final recorder = ui.PictureRecorder();
  final canvas = Canvas(
    recorder,
    Rect.fromLTWH(
      0,
      0,
      EpdProtocol.frameWidth.toDouble(),
      EpdProtocol.frameHeight.toDouble(),
    ),
  );
  paintContent(canvas, spec);
  final picture = recorder.endRecording();
  final image = await picture.toImage(
    EpdProtocol.frameWidth,
    EpdProtocol.frameHeight,
  );
  picture.dispose();
  final data = await image.toByteData(format: ui.ImageByteFormat.rawRgba);
  image.dispose();
  if (data == null) {
    throw StateError('picture.toImage → toByteData failed');
  }
  return compute(
    encodeFrameBytes,
    EncodeArgs(data.buffer.asUint8List(), dither, invert),
  );
}

/// 解码图片并限制最长边（大图二次按比例解码，防 4K 照片解码内存峰值）。
///
/// [maxSide] 默认 1248 = 3x416（目标视口 3 倍采样，缩放质量足够）。
Future<ui.Image> decodeImageCapped(
  Uint8List bytes, {
  int maxSide = 1248,
}) async {
  var codec = await ui.instantiateImageCodec(bytes);
  var frame = await codec.getNextFrame();
  var image = frame.image;
  final longest = math.max(image.width, image.height);
  if (longest > maxSide) {
    final landscape = image.width >= image.height;
    final codec2 = await ui.instantiateImageCodec(
      bytes,
      targetWidth: landscape ? maxSide : null,
      targetHeight: landscape ? null : maxSide,
    );
    final frame2 = await codec2.getNextFrame();
    image.dispose();
    image = frame2.image;
  }
  return image;
}
