/// 编辑发送页：文本/图片 → 预览 → 渲染打包 → POST /api/display
library;

import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:image_picker/image_picker.dart';
import 'package:provider/provider.dart';

import '../../services/http_device_client.dart';
import '../../services/render/frame_painter.dart';
import '../../state/device_controller.dart';
import 'preview_widgets.dart';

class ComposePage extends StatefulWidget {
  const ComposePage({super.key});

  @override
  State<ComposePage> createState() => _ComposePageState();
}

class _ComposePageState extends State<ComposePage> {
  final _textCtrl = TextEditingController(text: 'Hello InkWord 你好墨水屏');

  ContentType _mode = ContentType.text;
  double _fontSize = 24;
  Rotation _rotation = Rotation.auto;
  bool _dither = true;
  bool _invert = false;

  ui.Image? _image;
  int _imgW = 0;
  int _imgH = 0;

  /// null=空闲；'渲染中'/'发送中' 展示进度
  String? _busy;

  ContentSpec get _spec => ContentSpec(
    type: _mode,
    text: _textCtrl.text,
    fontSize: _fontSize,
    image: _image,
    imageWidth: _imgW,
    imageHeight: _imgH,
    rotation: _rotation,
  );

  @override
  void dispose() {
    _textCtrl.dispose();
    _image?.dispose();
    super.dispose();
  }

  Future<void> _pickImage() async {
    final picker = ImagePicker();
    final file = await picker.pickImage(source: ImageSource.gallery);
    if (file == null) return;
    final bytes = await file.readAsBytes();
    final img = await decodeImageCapped(bytes);
    if (!mounted) {
      img.dispose();
      return;
    }
    setState(() {
      _image?.dispose();
      _image = img;
      _imgW = img.width;
      _imgH = img.height;
      _mode = ContentType.image;
    });
  }

  Future<void> _send() async {
    final client = context.read<DeviceController>().client;
    if (client == null) {
      _toast('未连接设备，请先在“设备”页连接');
      return;
    }
    if (_mode == ContentType.image && _image == null) {
      _toast('请先选择图片');
      return;
    }
    setState(() => _busy = '渲染中');
    try {
      final Uint8List frame = await renderSpecToFrame(
        _spec,
        dither: _dither,
        invert: _invert,
      );
      setState(() => _busy = '发送中');
      await client.postDisplay(frame);
      if (mounted) _toast('已发送，墨水屏整帧刷新完成');
    } on DeviceBusyException catch (e) {
      if (mounted) _toast('$e');
    } on DeviceUnreachableException catch (e) {
      if (mounted) {
        _toast('$e（设备 IP 可能已变化，请回设备页重新发现）');
      }
    } catch (e) {
      if (mounted) _toast('发送失败：$e');
    } finally {
      if (mounted) setState(() => _busy = null);
    }
  }

  void _toast(String msg) {
    ScaffoldMessenger.of(context)
      ..hideCurrentSnackBar()
      ..showSnackBar(
        SnackBar(content: Text(msg), duration: const Duration(seconds: 3)),
      );
  }

  @override
  Widget build(BuildContext context) {
    final dev = context.watch<DeviceController>();
    return Scaffold(
      appBar: AppBar(
        title: const Text('发送到墨水屏'),
        actions: [
          Padding(
            padding: const EdgeInsets.only(right: 12),
            child: Center(
              child: Text(
                dev.connected ? (dev.host ?? '') : '未连接',
                style: TextStyle(
                  fontSize: 13,
                  color: dev.connected ? Colors.green.shade700 : Colors.red,
                ),
              ),
            ),
          ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.all(12),
        children: [
          _buildModeSelector(),
          const SizedBox(height: 8),
          if (_mode == ContentType.text) _buildTextPanel(),
          if (_mode == ContentType.image) _buildImagePanel(),
          _buildRotationSelector(),
          _buildSwitches(),
          const Divider(),
          _buildPreviews(),
          const SizedBox(height: 12),
          FilledButton(
            onPressed: _busy == null ? _send : null,
            child: Text(_busy == null ? '发送到墨水屏' : '$_busy...'),
          ),
          const SizedBox(height: 24),
        ],
      ),
    );
  }

  Widget _buildModeSelector() => SegmentedButton<ContentType>(
    segments: const [
      ButtonSegment(
        value: ContentType.text,
        label: Text('文本'),
        icon: Icon(Icons.text_fields),
      ),
      ButtonSegment(
        value: ContentType.image,
        label: Text('图片'),
        icon: Icon(Icons.image),
      ),
    ],
    selected: {_mode},
    onSelectionChanged: (s) => setState(() => _mode = s.first),
  );

  Widget _buildTextPanel() => Padding(
    padding: const EdgeInsets.only(top: 8),
    child: Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        TextField(
          controller: _textCtrl,
          maxLines: 4,
          decoration: const InputDecoration(
            labelText: '文本内容',
            border: OutlineInputBorder(),
            hintText: '支持中英文，自动折行居中显示',
          ),
          onChanged: (_) => setState(() {}),
        ),
        const SizedBox(height: 8),
        Row(
          children: [
            const Text('字号'),
            const SizedBox(width: 12),
            DropdownButton<double>(
              value: _fontSize,
              items: const [
                DropdownMenuItem(value: 16, child: Text('16')),
                DropdownMenuItem(value: 24, child: Text('24')),
                DropdownMenuItem(value: 32, child: Text('32')),
                DropdownMenuItem(value: 48, child: Text('48')),
              ],
              onChanged: (v) => setState(() => _fontSize = v ?? 24),
            ),
          ],
        ),
      ],
    ),
  );

  Widget _buildImagePanel() => Padding(
    padding: const EdgeInsets.only(top: 8),
    child: Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Row(
          children: [
            OutlinedButton.icon(
              onPressed: _pickImage,
              icon: const Icon(Icons.photo_library),
              label: const Text('选择图片'),
            ),
            const SizedBox(width: 12),
            Expanded(
              child: Text(
                _image == null
                    ? '未选择图片'
                    : '$_imgW x $_imgH px（自动旋转：${_imgW > _imgH ? "横图将转 90° 横屏显示" : "竖图保持竖屏"}）',
                style: const TextStyle(fontSize: 13, color: Colors.black54),
              ),
            ),
          ],
        ),
      ],
    ),
  );

  Widget _buildRotationSelector() => Padding(
    padding: const EdgeInsets.symmetric(vertical: 8),
    child: Row(
      children: [
        const Text('旋转'),
        const SizedBox(width: 12),
        Expanded(
          child: DropdownButton<Rotation>(
            value: _rotation,
            isExpanded: true,
            items: const [
              DropdownMenuItem(value: Rotation.auto, child: Text('自动（横图转横屏）')),
              DropdownMenuItem(value: Rotation.r0, child: Text('0°（竖屏）')),
              DropdownMenuItem(value: Rotation.r90, child: Text('90°（横屏）')),
              DropdownMenuItem(value: Rotation.r180, child: Text('180°')),
              DropdownMenuItem(value: Rotation.r270, child: Text('270°（横屏）')),
            ],
            onChanged: (v) => setState(() => _rotation = v ?? Rotation.auto),
          ),
        ),
      ],
    ),
  );

  Widget _buildSwitches() => Row(
    children: [
      Switch(value: _dither, onChanged: (v) => setState(() => _dither = v)),
      const Text('抖动（灰度模拟）'),
      const SizedBox(width: 16),
      Switch(value: _invert, onChanged: (v) => setState(() => _invert = v)),
      const Text('反色'),
    ],
  );

  Widget _buildPreviews() => Column(
    crossAxisAlignment: CrossAxisAlignment.start,
    children: [
      const Text('预览（竖屏 240x416）：'),
      const SizedBox(height: 6),
      Center(
        child: SizedBox(
          height: 300,
          width: double.infinity,
          child: EpdPreview(spec: _spec),
        ),
      ),
      const SizedBox(height: 12),
      const Text('设备横屏视角（横持设备时的效果，416x240）：'),
      const SizedBox(height: 6),
      Center(
        child: SizedBox(
          height: 160,
          width: double.infinity,
          child: EpdLandscapePreview(spec: _spec),
        ),
      ),
    ],
  );
}
