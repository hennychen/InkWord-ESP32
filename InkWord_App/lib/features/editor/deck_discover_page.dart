/// 发现卡组页（v2.0 #3 UGC 分享首增量）
///
/// 浏览他人公开分享的卡组（SharedAt 倒序 + 名称搜索 + 分页），
/// 一键导入（fork 深拷贝）为独立副本归自己所有；导入后走既有
/// LAN 推送 / 增量同步通道下发设备（设备零改动）。离线优先红线：
/// 云端不可达只影响本页，不影响本地编辑与推送。
library;

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../../services/cloud_client.dart';
import '../../state/account_controller.dart';
import 'deck_detail_page.dart';
import 'deck_editor_page.dart' show payloadTypeLabel;

class DeckDiscoverPage extends StatefulWidget {
  const DeckDiscoverPage({super.key});

  @override
  State<DeckDiscoverPage> createState() => _DeckDiscoverPageState();
}

class _DeckDiscoverPageState extends State<DeckDiscoverPage> {
  final List<CloudDeck> _items = [];
  final Set<String> _forkedSrcIds = {}; // 会话内已导入的源 id（防重复提示）
  final _searchCtrl = TextEditingController();
  String? _error;
  bool _loading = false;
  bool _hasMore = false;
  int _page = 1;
  String? _keyword;

  CloudClient? get _cloud => context.read<AccountController>().client;

  @override
  void initState() {
    super.initState();
    Future.microtask(() => _load(reset: true));
  }

  @override
  void dispose() {
    _searchCtrl.dispose();
    super.dispose();
  }

  Future<void> _load({required bool reset}) async {
    final c = _cloud;
    if (c == null) return;
    if (reset) _page = 1;
    setState(() => _loading = true);
    try {
      final r = await c.sharedDecks(page: _page, q: _keyword);
      if (!mounted) return;
      setState(() {
        if (reset) _items.clear();
        _items.addAll(r.items);
        _hasMore = r.hasMore;
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

  Future<void> _fork(CloudDeck d) async {
    final c = _cloud;
    if (c == null) return;
    try {
      final copy = await c.forkDeck(d.id);
      setState(() => _forkedSrcIds.add(d.id));
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text('已导入「${copy.name}」（${copy.itemCount} 条）到我的卡组'),
          action: SnackBarAction(
            label: '查看',
            onPressed: () => Navigator.push(
              context,
              MaterialPageRoute(builder: (_) => DeckDetailPage(cloud: copy)),
            ).then((_) => _load(reset: true)),
          ),
        ),
      );
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(
          context,
        ).showSnackBar(SnackBar(content: Text('导入失败：$e')));
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    final loggedIn = context.watch<AccountController>().loggedIn;
    return Scaffold(
      appBar: AppBar(title: const Text('发现卡组')),
      body: !loggedIn
          ? const Center(child: Text('登录后可浏览与导入他人分享的卡组'))
          : RefreshIndicator(
              onRefresh: () => _load(reset: true),
              child: ListView(
                children: [
                  Padding(
                    padding: const EdgeInsets.fromLTRB(12, 10, 12, 4),
                    child: TextField(
                      controller: _searchCtrl,
                      onSubmitted: (v) {
                        _keyword = v.trim().isEmpty ? null : v.trim();
                        _load(reset: true);
                      },
                      decoration: InputDecoration(
                        hintText: '搜索卡组名称，回车搜索',
                        prefixIcon: const Icon(Icons.search),
                        suffixIcon: _keyword == null
                            ? null
                            : IconButton(
                                icon: const Icon(Icons.close),
                                onPressed: () {
                                  _searchCtrl.clear();
                                  _keyword = null;
                                  _load(reset: true);
                                },
                              ),
                        border: const OutlineInputBorder(),
                        isDense: true,
                      ),
                    ),
                  ),
                  if (_error != null)
                    Padding(
                      padding: const EdgeInsets.all(16),
                      child: Text(
                        '加载失败：$_error',
                        style: const TextStyle(color: Colors.red, fontSize: 13),
                      ),
                    )
                  else if (!_loading && _items.isEmpty)
                    const Padding(
                      padding: EdgeInsets.all(24),
                      child: Center(
                        child: Text(
                          '暂无分享卡组\n（其他用户在编辑器公开分享后会出现在这里）',
                          textAlign: TextAlign.center,
                          style: TextStyle(color: Colors.black54),
                        ),
                      ),
                    )
                  else
                    for (final d in _items)
                      ListTile(
                        leading: const Icon(Icons.public),
                        title: Text(d.name),
                        subtitle: Text(
                          '${payloadTypeLabel(d.payloadType)} · ${d.itemCount} 条'
                          '${d.ownerName == null || d.ownerName!.isEmpty ? "" : " · ${d.ownerName}"}',
                        ),
                        isThreeLine: false,
                        trailing: _forkedSrcIds.contains(d.id)
                            ? const Text(
                                '已导入',
                                style: TextStyle(
                                  fontSize: 12,
                                  color: Colors.green,
                                ),
                              )
                            : FilledButton.tonal(
                                onPressed: () => _fork(d),
                                child: const Text('导入'),
                              ),
                      ),
                  if (_loading)
                    const Padding(
                      padding: EdgeInsets.all(16),
                      child: Center(
                        child: SizedBox(
                          width: 22,
                          height: 22,
                          child: CircularProgressIndicator(strokeWidth: 2.5),
                        ),
                      ),
                    )
                  else if (_hasMore)
                    Padding(
                      padding: const EdgeInsets.all(12),
                      child: OutlinedButton(
                        onPressed: () {
                          _page++;
                          _load(reset: false);
                        },
                        child: const Text('加载更多'),
                      ),
                    ),
                ],
              ),
            ),
    );
  }
}
