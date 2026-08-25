/// 卡组详情/条目编辑页（v1.5 T5.3）
///
/// 两种来源：云端卡组（CloudDeck，条目即时 CRUD 写回）与本地草稿
/// （LocalDraft，纯本地编辑，可整体保存云端 / 推送设备）。
/// 推送 = LAN 直传复用 T3.4 uploadDeck（words.json 全量覆盖
/// SD decks/{id}/；manifest 登记版式——设备零账户感知）。
library;

import 'dart:convert';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:provider/provider.dart';

import '../../services/cloud_client.dart';
import '../../state/device_controller.dart';
import 'card_item_sheet.dart';
import 'deck_codec.dart';
import 'deck_editor_page.dart' show payloadTypeLabel;

/// 本地草稿（未保存云端的卡组）
class LocalDraft {
  LocalDraft({
    required this.name,
    required this.subjectCode,
    required this.payloadType,
    required this.deviceId,
    this.description = '',
  });

  String name;
  String subjectCode;
  String payloadType;
  String deviceId; // LAN 推送 id（1~7 字符，设备约束同源）
  String description;
  final List<EditorItem> items = [];
}

class DeckDetailPage extends StatefulWidget {
  const DeckDetailPage({super.key, this.cloud, this.draft})
    : assert(cloud != null || draft != null);

  final CloudDeck? cloud;
  final LocalDraft? draft;

  @override
  State<DeckDetailPage> createState() => _DeckDetailPageState();
}

class _DeckDetailPageState extends State<DeckDetailPage> {
  final List<EditorItem> _items = [];
  bool _loading = false;
  bool _busy = false;

  String get _name => widget.cloud?.name ?? widget.draft!.name;
  String get _payloadType =>
      widget.cloud?.payloadType ?? widget.draft!.payloadType;

  /// 云端客户端（登录后由 ProxyProvider 派生；草稿模式可为 null）
  CloudClient? _cloudOf(BuildContext ctx) => ctx.read<CloudClient?>();

  @override
  void initState() {
    super.initState();
    final deckId = widget.cloud?.id;
    if (deckId != null) {
      _loading = true;
      final client = _cloudOf(context); // initState 同步 read（无 async gap）
      Future.microtask(() async {
        try {
          final items =
              await client?.fetchItems(deckId) ?? const <EditorItem>[];
          if (!mounted) return;
          setState(() {
            _items.addAll(items);
            _loading = false;
          });
        } catch (e) {
          if (!mounted) return;
          setState(() => _loading = false);
          ScaffoldMessenger.of(
            context,
          ).showSnackBar(SnackBar(content: Text('条目加载失败：$e')));
        }
      });
    } else {
      _items.addAll(widget.draft!.items);
    }
  }

  Future<void> _addItem() async {
    final it = await showCardItemSheet(context, payloadType: _payloadType);
    if (it == null) return;
    setState(() => _items.add(it));
    await _syncNew(it);
  }

  Future<void> _editItem(int index) async {
    final old = _items[index];
    final c = _cloudOf(context); // await 前捕（避免 async gap）
    final it = await showCardItemSheet(
      context,
      payloadType: _payloadType,
      initial: old,
    );
    if (it == null) return;
    setState(() => _items[index] = it);
    if (c != null && widget.cloud != null && it.id != null) {
      try {
        await c.updateItem(widget.cloud!.id, it);
      } catch (e) {
        _err('保存失败：$e');
      }
    }
  }

  Future<void> _removeItem(int index) async {
    final it = _items[index];
    setState(() => _items.removeAt(index));
    if (widget.draft != null) return; // 草稿仅本地
    final c = _cloudOf(context); // setState 同步段捕（无 await gap）
    if (c != null && it.id != null) {
      try {
        await c.deleteItem(widget.cloud!.id, it.id!);
      } catch (e) {
        _err('删除失败：$e（已本地移除，重进页面恢复）');
      }
    }
  }

  /// 新增条目即时写云（草稿跳过）
  Future<void> _syncNew(EditorItem it) async {
    final c = _cloudOf(context); // 调用点在 setState 同步段
    if (c == null || widget.cloud == null) return;
    try {
      await c.addItems(widget.cloud!.id, [it]);
    } catch (e) {
      _err('保存失败：$e（条目保留在列表，CSV 重导可补）');
    }
  }

  Future<void> _importCsv() async {
    final c = _cloudOf(context); // await 前捕（避免 async gap）
    final picked = await FilePicker.platform.pickFiles(
      type: FileType.custom,
      allowedExtensions: ['csv', 'txt'],
      withData: true,
    );
    final file = picked?.files.singleOrNull;
    final bytes = file?.bytes;
    if (file == null || bytes == null) return;

    final text = utf8.decode(bytes, allowMalformed: true);
    final imported = parseCsv(text);
    if (imported.isEmpty) {
      _err('未解析到有效条目（格式：正面,背面[,音标,例句]）');
      return;
    }
    setState(() => _items.addAll(imported));

    if (c != null && widget.cloud != null) {
      try {
        await c.addItems(widget.cloud!.id, imported);
        _ok('已导入 ${imported.length} 条并保存云端');
      } catch (e) {
        _err('导入成功 ${imported.length} 条，云端保存失败：$e');
      }
    } else {
      _ok('已导入 ${imported.length} 条（保存云端或推送设备生效）');
    }
  }

  /// 草稿整体保存云端（createDeck 全量）
  Future<void> _saveDraftToCloud() async {
    final draft = widget.draft;
    if (draft == null || _items.isEmpty) {
      _err('无条目可保存');
      return;
    }
    final c = _cloudOf(context); // 同步段捕
    if (c == null) {
      _err('未登录（回到编辑器首页登录后再保存）');
      return;
    }
    setState(() => _busy = true);
    try {
      final deck = await c.createDeck(
        subjectCode: draft.subjectCode,
        name: draft.name,
        payloadType: draft.payloadType,
        description: draft.description,
        items: _items,
      );
      _ok('已保存云端（code ${deck.code}）');
      if (mounted) {
        // 草稿已上云：跳转云端详情（替换当前页，草稿留给首页清理）
        Navigator.pushReplacement(
          context,
          MaterialPageRoute(
            builder: (_) => DeckDetailPage(
              cloud: CloudDeck(
                id: deck.id,
                code: deck.code,
                name: deck.name,
                payloadType: deck.payloadType,
                subjectCode: draft.subjectCode,
              ),
            ),
          ),
        );
      }
    } catch (e) {
      _err('保存失败：$e');
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  /// LAN 推送到设备（复用 T3.4 uploadDeck 全量覆盖）
  Future<void> _pushToDevice() async {
    if (_items.isEmpty) {
      _err('无条目可推送');
      return;
    }
    final device = context.read<DeviceController>();
    final client = device.client;
    if (client == null) {
      _err('未连接设备（设备页先连接）');
      return;
    }

    final messenger = ScaffoldMessenger.of(context);
    final defaultId = widget.cloud?.code ?? widget.draft!.deviceId;
    final idCtrl = TextEditingController(
      text: defaultId,
    )..selection = TextSelection(baseOffset: 0, extentOffset: defaultId.length);
    final confirmed = await showDialog<(String, String)>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('推送到设备'),
        content: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Text(
              '共 ${_items.length} 条 · ${payloadTypeLabel(_payloadType)}\n'
              '覆盖设备 SD decks/<id>/words.json',
            ),
            const SizedBox(height: 12),
            TextField(
              controller: idCtrl,
              maxLength: 7,
              inputFormatters: [
                FilteringTextInputFormatter.allow(RegExp(r'[0-9a-zA-Z_-]')),
              ],
              decoration: const InputDecoration(
                labelText: '设备词书 id（1~7 位）',
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
              Navigator.pop(ctx, (id, _name));
            },
            child: const Text('推送'),
          ),
        ],
      ),
    );
    if (confirmed == null) return;

    setState(() => _busy = true);
    try {
      final bytes = buildWordsJson(_items);
      await client.uploadDeck(
        confirmed.$1,
        confirmed.$2,
        _items.length,
        bytes,
        type: _payloadType,
      );
      if (widget.draft != null) widget.draft!.deviceId = confirmed.$1;
      messenger.showSnackBar(
        SnackBar(
          content: Text(
            '已推送（${confirmed.$1}，${_items.length} 条）。'
            '到「词书」页切换开始学习',
          ),
        ),
      );
    } catch (e) {
      if (mounted) {
        messenger.showSnackBar(SnackBar(content: Text('推送失败：$e')));
      }
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _deleteDeck() async {
    final cloud = widget.cloud;
    final c = _cloudOf(context); // 同步段捕
    final okDelete = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: Text('删除「$_name」？'),
        content: const Text('云端卡组与全部条目将删除（设备 SD 拷贝不受影响）'),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(ctx, false),
            child: const Text('取消'),
          ),
          FilledButton(
            onPressed: () => Navigator.pop(ctx, true),
            child: const Text('删除'),
          ),
        ],
      ),
    );
    if (okDelete != true) return;
    if (cloud != null && c != null) {
      try {
        await c.deleteDeck(cloud.id);
      } catch (e) {
        _err('删除失败：$e');
        return;
      }
    }
    if (mounted) Navigator.pop(context, true);
  }

  void _ok(String msg) {
    if (mounted) {
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(msg)));
    }
  }

  void _err(String msg) {
    if (mounted) {
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(msg)));
    }
  }

  @override
  Widget build(BuildContext context) {
    final connected = context.watch<DeviceController>().connected;
    return Scaffold(
      appBar: AppBar(
        title: Text(_name),
        actions: [
          IconButton(
            tooltip: 'CSV 导入（Excel 另存 CSV UTF-8）',
            onPressed: _busy ? null : _importCsv,
            icon: const Icon(Icons.upload_file),
          ),
          IconButton(
            tooltip: connected ? '推送到设备' : '推送（未连接设备）',
            onPressed: _busy ? null : _pushToDevice,
            icon: Icon(
              Icons.send_to_mobile,
              color: connected ? null : Colors.black26,
            ),
          ),
          PopupMenuButton<String>(
            onSelected: (v) {
              if (v == 'cloud') _saveDraftToCloud();
              if (v == 'delete') _deleteDeck();
            },
            itemBuilder: (_) => [
              if (widget.draft != null)
                const PopupMenuItem(value: 'cloud', child: Text('保存到云端（需登录）')),
              PopupMenuItem(
                value: 'delete',
                child: Text(widget.cloud != null ? '删除卡组' : '舍弃草稿'),
              ),
            ],
          ),
        ],
      ),
      floatingActionButton: FloatingActionButton.extended(
        onPressed: _busy ? null : _addItem,
        icon: const Icon(Icons.add),
        label: const Text('新增条目'),
      ),
      body: _loading
          ? const Center(child: CircularProgressIndicator())
          : _items.isEmpty
          ? const _EmptyHint()
          : ListView.builder(
              itemCount: _items.length,
              itemBuilder: (_, i) {
                final it = _items[i];
                return Dismissible(
                  key: ValueKey(it.id ?? 'local-$i-${it.front}'),
                  background: Container(
                    color: Colors.red,
                    alignment: Alignment.centerRight,
                    padding: const EdgeInsets.only(right: 20),
                    child: const Icon(Icons.delete, color: Colors.white),
                  ),
                  direction: DismissDirection.endToStart,
                  onDismissed: (_) => _removeItem(i),
                  child: ListTile(
                    title: Text(
                      it.front,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                    ),
                    subtitle: Text(
                      it.back,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                    ),
                    onTap: () => _editItem(i),
                  ),
                );
              },
            ),
    );
  }
}

class _EmptyHint extends StatelessWidget {
  const _EmptyHint();

  @override
  Widget build(BuildContext context) {
    return Center(
      child: Column(
        mainAxisAlignment: MainAxisAlignment.center,
        children: [
          const Icon(Icons.post_add, size: 56, color: Colors.black26),
          const SizedBox(height: 12),
          const Text(
            '暂无条目：新增或 CSV 导入',
            style: TextStyle(color: Colors.black45),
          ),
          const SizedBox(height: 6),
          Text(
            'CSV 列序：正面,背面[,音标,例句]（Excel 另存 CSV UTF-8）',
            style: Theme.of(
              context,
            ).textTheme.bodySmall?.copyWith(color: Colors.black38),
          ),
        ],
      ),
    );
  }
}
