/// 词书管理页（v1.3 T3.4）：列表 / 切换 / LAN 直传上传
///
/// - 列表与切换走设备 GET /api/decks + POST /api/deck/active
///   （设备侧复用菜单切书编排：NVS 记录 / 词库重载 / 学习状态作废 /
///   阅读进度隔离 / 归位闪卡）
/// - 上传：file_picker 选 words.json 原文，POST /api/deck/upload
///   流式落 SD decks 目录（按 id 子目录）+ manifest 登记
/// - AppBar 菜单预留「卡组编辑器」二期入口位（v1.4）
library;

import 'dart:convert';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:provider/provider.dart';

import '../../services/http_device_client.dart';
import '../../state/device_controller.dart';
import '../editor/deck_editor_page.dart';

class DeckPage extends StatefulWidget {
  const DeckPage({super.key});

  @override
  State<DeckPage> createState() => _DeckPageState();
}

class _DeckPageState extends State<DeckPage> {
  String? _error;
  bool _loading = true;
  bool _switching = false;
  String _active = '';
  List<DeckInfo> _decks = const [];

  DeviceHttpClient? get _client => context.read<DeviceController>().client;

  @override
  void initState() {
    super.initState();
    _reload();
  }

  Future<void> _reload() async {
    final c = _client;
    if (c == null) {
      setState(() {
        _loading = false;
        _error = null;
      });
      return;
    }
    setState(() => _loading = true);
    try {
      final (active, decks) = await c.fetchDecks();
      if (!mounted) return;
      setState(() {
        _active = active;
        _decks = decks;
        _error = null;
        _loading = false;
      });
    } catch (e) {
      if (!mounted) return;
      setState(() {
        _error = e.toString();
        _loading = false;
      });
    }
  }

  Future<void> _switch(String id) async {
    if (_switching || id == _active) return;
    final c = _client;
    if (c == null) return;
    setState(() => _switching = true);
    final messenger = ScaffoldMessenger.of(context);
    try {
      await c.postDeckActive(id);
      await _reload();
      messenger.showSnackBar(
        SnackBar(content: Text('已切换词书（学习进度按词书隔离，今日统计保留）')),
      );
    } catch (e) {
      messenger.showSnackBar(SnackBar(content: Text('切换失败：$e')));
    } finally {
      if (mounted) setState(() => _switching = false);
    }
  }

  Future<void> _upload() async {
    final c = _client;
    if (c == null) return;
    final messenger = ScaffoldMessenger.of(context);
    final picked = await FilePicker.platform.pickFiles(
      type: FileType.custom,
      allowedExtensions: ['json'],
      withData: true,
    );
    final file = picked?.files.singleOrNull;
    if (file == null) return;
    final bytes = file.bytes;
    if (bytes == null || bytes.isEmpty) return;

    // 词条数（信息展示用，供 manifest count 登记；解析失败不强阻）
    int count = 0;
    try {
      final j = jsonDecode(utf8.decode(bytes)) as Map<String, dynamic>;
      count = (j['words'] as List?)?.length ?? 0;
    } catch (_) {
      /* 非法 JSON 由设备侧首块校验拦 */
    }

    final defaultId = file.name.replaceAll(
      RegExp(r'\.json$', caseSensitive: false),
      '',
    );
    final form = await _showUploadDialog(
      fileName: file.name,
      sizeKb: bytes.length ~/ 1024,
      wordCount: count,
      defaultId: defaultId,
    );
    if (form == null) return;

    setState(() => _switching = true);
    try {
      await c.uploadDeck(form.$1, form.$2, count, bytes);
      await _reload();
      messenger.showSnackBar(
        SnackBar(content: Text('词书已上传（${form.$2}，$count 词）')),
      );
    } catch (e) {
      messenger.showSnackBar(SnackBar(content: Text('上传失败：$e')));
    } finally {
      if (mounted) setState(() => _switching = false);
    }
  }

  /// 上传表单：返回 (id, name)；校验 id 格式（1~7 位 [0-9a-zA-Z_-]）
  Future<(String, String)?> _showUploadDialog({
    required String fileName,
    required int sizeKb,
    required int wordCount,
    required String defaultId,
  }) {
    final idCtrl = TextEditingController()
      ..text = defaultId
          .replaceAll(RegExp(r'[^0-9a-zA-Z_-]'), '')
          .substring(
            0,
            defaultId
                .replaceAll(RegExp(r'[^0-9a-zA-Z_-]'), '')
                .length
                .clamp(0, 7),
          );
    final nameCtrl = TextEditingController();
    return showDialog<(String, String)>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('上传词书'),
        content: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Text(
              '$fileName（${sizeKb}KB · $wordCount 词）',
              style: const TextStyle(fontSize: 13, color: Colors.black54),
            ),
            const SizedBox(height: 12),
            TextField(
              controller: idCtrl,
              maxLength: 7,
              inputFormatters: [
                FilteringTextInputFormatter.allow(RegExp(r'[0-9a-zA-Z_-]')),
              ],
              decoration: const InputDecoration(
                labelText: '短 id（1~7 位字母数字）',
                hintText: '如 cet4',
                border: OutlineInputBorder(),
              ),
            ),
            TextField(
              controller: nameCtrl,
              decoration: const InputDecoration(
                labelText: '显示名（可选，默认同 id）',
                hintText: '如 CET-4 核心',
                border: OutlineInputBorder(),
              ),
            ),
          ],
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(ctx),
            child: const Text('取消'),
          ),
          FilledButton(
            onPressed: () {
              final id = idCtrl.text.trim();
              if (id.isEmpty) return;
              final name = nameCtrl.text.trim();
              Navigator.pop(ctx, (id, name.isEmpty ? id : name));
            },
            child: const Text('上传'),
          ),
        ],
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final connected = context.watch<DeviceController>().connected;
    return Scaffold(
      appBar: AppBar(
        title: const Text('词书管理'),
        actions: [
          // v1.5 T5.3：卡组编辑器入口（版式模板/条目/CSV 导入/推送）
          PopupMenuButton<String>(
            onSelected: (v) {
              if (v == 'editor') {
                Navigator.push(
                  context,
                  MaterialPageRoute(builder: (_) => const DeckEditorPage()),
                );
              }
            },
            itemBuilder: (_) => const [
              PopupMenuItem(value: 'editor', child: Text('卡组编辑器')),
            ],
          ),
        ],
      ),
      floatingActionButton: connected
          ? FloatingActionButton.extended(
              onPressed: _switching ? null : _upload,
              icon: const Icon(Icons.upload_file),
              label: const Text('上传词书'),
            )
          : null,
      body: !connected
          ? const _HintCard(text: '未连接设备：请先在「设备」页连接（BLE / mDNS / IP）')
          : RefreshIndicator(
              onRefresh: _reload,
              child: _loading
                  ? const Center(child: CircularProgressIndicator())
                  : _error != null
                  ? ListView(children: [_HintCard(text: '加载失败：$_error')])
                  : ListView(
                      children: [
                        for (final d in _decks)
                          RadioListTile<String>(
                            value: d.id,
                            groupValue: _active,
                            title: Text(d.name),
                            subtitle: Text(
                              d.id.isEmpty
                                  ? '设备默认（words.json / 内置）'
                                  : '${d.id} · ${d.count} 词',
                            ),
                            onChanged: _switching ? null : (_) => _switch(d.id),
                          ),
                        if (_decks.length <= 1)
                          const _HintCard(
                            text:
                                '暂无自定义词书：上传 words.json 格式词库'
                                '（顶层 {"words":[...]}，字段与设备默认一致）',
                          ),
                      ],
                    ),
            ),
    );
  }
}

class _HintCard extends StatelessWidget {
  final String text;
  const _HintCard({required this.text});

  @override
  Widget build(BuildContext context) {
    return Card(
      margin: const EdgeInsets.all(12),
      child: Padding(
        padding: const EdgeInsets.all(14),
        child: Text(
          text,
          style: const TextStyle(fontSize: 13, color: Colors.black54),
        ),
      ),
    );
  }
}
