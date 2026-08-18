/// 发送内容预览组件
///
/// - [EpdPreview]：竖屏缓冲视角（240x416，最终帧内容）
/// - [EpdLandscapePreview]：设备横屏视角（416x240，竖屏缓冲逆时针转 90°，
///   对齐网页端 devView / 固件 GFX rotation=1）
library;

import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../../core/epd_protocol.dart';
import '../../services/render/frame_painter.dart';

class EpdPreview extends StatelessWidget {
  final ContentSpec spec;

  const EpdPreview({super.key, required this.spec});

  @override
  Widget build(BuildContext context) {
    return FittedBox(
      fit: BoxFit.contain,
      child: RepaintBoundary(
        child: SizedBox(
          width: EpdProtocol.frameWidth.toDouble(),
          height: EpdProtocol.frameHeight.toDouble(),
          child: CustomPaint(
            painter: _ContentPainter(spec),
            size: const Size(
              EpdProtocol.frameWidth * 1.0,
              EpdProtocol.frameHeight * 1.0,
            ),
          ),
        ),
      ),
    );
  }
}

class EpdLandscapePreview extends StatelessWidget {
  final ContentSpec spec;

  const EpdLandscapePreview({super.key, required this.spec});

  @override
  Widget build(BuildContext context) {
    return FittedBox(
      fit: BoxFit.contain,
      child: RepaintBoundary(
        child: SizedBox(
          width: EpdProtocol.frameHeight.toDouble(),
          height: EpdProtocol.frameWidth.toDouble(),
          child: CustomPaint(
            painter: _LandscapePainter(spec),
            size: Size(
              EpdProtocol.frameHeight * 1.0,
              EpdProtocol.frameWidth * 1.0,
            ),
          ),
        ),
      ),
    );
  }
}

class _ContentPainter extends CustomPainter {
  final ContentSpec spec;
  _ContentPainter(this.spec);

  @override
  void paint(Canvas canvas, Size size) {
    final s = size.width / EpdProtocol.frameWidth;
    canvas.save();
    canvas.scale(s);
    paintContent(canvas, spec);
    canvas.restore();
  }

  @override
  bool shouldRepaint(_ContentPainter oldDelegate) =>
      !identical(oldDelegate.spec, spec);
}

class _LandscapePainter extends CustomPainter {
  final ContentSpec spec;
  _LandscapePainter(this.spec);

  @override
  void paint(Canvas canvas, Size size) {
    final s = size.width / EpdProtocol.frameHeight; // 416
    canvas.save();
    canvas.scale(s);
    canvas.translate(EpdProtocol.frameHeight / 2, EpdProtocol.frameWidth / 2);
    canvas.rotate(-math.pi / 2);
    paintContent(canvas, spec);
    canvas.restore();
  }

  @override
  bool shouldRepaint(_LandscapePainter oldDelegate) =>
      !identical(oldDelegate.spec, spec);
}
