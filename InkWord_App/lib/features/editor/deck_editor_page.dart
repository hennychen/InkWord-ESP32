/// 卡组编辑器主页（v1.5 T5.3）
///
/// 轻账户（ACCOUNT_MODEL_DECISION §四）：登录后我的卡组云端列表
/// （Deck.OwnerId 归属）；未登录可建本地草稿（离线优先红线——
/// LAN 推送不依赖账户）。新建 = 科目下拉 + 版式模板三选
/// （word/qa/poem，T4.3 card_layout 分派契约）+ 名称。
library;

import 'dart:math';

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../../services/cloud_client.dart';
import '../../state/account_controller.dart';
import 'deck_detail_page.dart';

class DeckEditorPage extends StatefulWidget {
  const DeckEditorPage({super.key});

  @override
  State<DeckEditorPage> createState() => _DeckEditorPageState();
}

class _DeckEditorPageState extends State<DeckEditorPage> {
  List<CloudDeck> _decks = const [];
  bool _loading = true;
  String? _error;
  final List<LocalDraft> _drafts = [];

  AccountController get _account => context.read<AccountController>();
  CloudClient? get _cloud => _account.client;

  @override
  void initState() {
    super.initState();
    Future.microtask(_reload);
  }

  Future<void> _reload() async {
    final c = _cloud;
    if (c == null) {
      if (mounted) setState(() => _loading = false);
      return;
    }
    setState(() => _loading = true);
    try {
      final decks = await c.myDecks();
      if (!mounted) return;
      setState(() {
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

  Future<void> _newDeck() async {
    final c = _cloud;
    final draft = await _showCreateDialog(c?.subjects());
    if (draft == null) return;

    if (c == null) {
      // 未登录：本地草稿（离线优先；后续可保存云端或直接推送设备）
      setState(() => _drafts.add(draft));
      _openDraft(draft);
      return;
    }
    try {
      final deck = await c.createDeck(
        subjectCode: draft.subjectCode,
        name: draft.name,
        payloadType: draft.payloadType,
      );
      await _reload();
      if (!mounted) return;
      _openDetail(
        CloudDeck(
          id: deck.id,
          code: deck.code,
          name: draft.name,
          payloadType: draft.payloadType,
          subjectCode: draft.subjectCode,
        ),
      );
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(
          context,
        ).showSnackBar(SnackBar(content: Text('创建失败：$e')));
      }
    }
  }

  Future<LocalDraft?> _showCreateDialog(
    Future<List<CloudSubject>>? subs,
  ) async {
    final nameCtrl = TextEditingController();
    var subjectCode = 'en';
    var payloadType = 'word-card';
    List<CloudSubject> subjects = [const CloudSubject(code: 'en', name: '英语')];
    if (subs != null) {
      try {
        subjects = await subs;
      } catch (_) {
        /* 云端不可达用默认 */
      }
    }
    if (!mounted) return null;
    return showDialog<LocalDraft>(
      context: context,
      builder: (ctx) => StatefulBuilder(
        builder: (ctx, setDlg) => AlertDialog(
          title: const Text('新建卡组'),
          content: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              TextField(
                controller: nameCtrl,
                decoration: const InputDecoration(
                  labelText: '卡组名称（必填）',
                  border: OutlineInputBorder(),
                ),
              ),
              const SizedBox(height: 12),
              DropdownButtonFormField<String>(
                value: subjectCode,
                items: [
                  for (final s in subjects)
                    DropdownMenuItem(value: s.code, child: Text(s.name)),
                ],
                onChanged: subjects.isEmpty
                    ? null
                    : (v) => setDlg(() => subjectCode = v ?? 'en'),
                decoration: const InputDecoration(
                  labelText: '科目',
                  border: OutlineInputBorder(),
                ),
              ),
              const SizedBox(height: 12),
              Text('版式模板', style: Theme.of(ctx).textTheme.labelLarge),
              for (final t in const [
                ('word-card', '单词卡：单词 / 音标 / 释义 / 例句'),
                ('qa-card', '问答卡：题面 / 答案（知识点）'),
                ('poem-card', '诗文卡：上句 / 下句（默写）'),
              ])
                RadioListTile<String>(
                  value: t.$1,
                  groupValue: payloadType,
                  onChanged: (v) => setDlg(() => payloadType = v ?? t.$1),
                  title: Text(t.$2, style: const TextStyle(fontSize: 13)),
                  dense: true,
                  contentPadding: EdgeInsets.zero,
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
                final name = nameCtrl.text.trim();
                if (name.isEmpty) return;
                Navigator.pop(
                  ctx,
                  LocalDraft(
                    name: name,
                    subjectCode: subjectCode,
                    payloadType: payloadType,
                    deviceId: _randomDeviceId(),
                  ),
                );
              },
              child: const Text('创建'),
            ),
          ],
        ),
      ),
    );
  }

  /// 本地草稿的设备 id（1~7 字符 [0-9a-zA-Z]，设备 deck id 约束同源）
  static String _randomDeviceId() {
    const chars = 'abcdefghjkmnpqrstuvwxyz23456789';
    final rng = Random();
    return String.fromCharCodes([
      for (var i = 0; i < 6; i++) chars.codeUnitAt(rng.nextInt(chars.length)),
    ]);
  }

  void _openDetail(CloudDeck deck) {
    Navigator.push(
      context,
      MaterialPageRoute(builder: (_) => DeckDetailPage(cloud: deck)),
    ).then((_) => _reload());
  }

  void _openDraft(LocalDraft draft) {
    Navigator.push(
      context,
      MaterialPageRoute(builder: (_) => DeckDetailPage(draft: draft)),
    ).then((_) => setState(() {}));
  }

  @override
  Widget build(BuildContext context) {
    final account = context.watch<AccountController>();
    return Scaffold(
      appBar: AppBar(
        title: const Text('卡组编辑器'),
        actions: [
          if (account.loggedIn)
            PopupMenuButton<String>(
              onSelected: (v) async {
                if (v == 'logout') await account.logout();
                if (v == 'server') await _showServerDialog(account);
              },
              itemBuilder: (_) => const [
                PopupMenuItem(value: 'server', child: Text('服务器地址')),
                PopupMenuItem(value: 'logout', child: Text('退出登录')),
              ],
              child: Padding(
                padding: const EdgeInsets.symmetric(horizontal: 14),
                child: Center(
                  child: Text(account.displayName ?? account.username ?? ''),
                ),
              ),
            )
          else
            TextButton(
              onPressed: () => _showServerDialog(account),
              child: const Text('服务器'),
            ),
        ],
      ),
      floatingActionButton: FloatingActionButton.extended(
        onPressed: _newDeck,
        icon: const Icon(Icons.add),
        label: const Text('新建卡组'),
      ),
      body: account.restoring
          ? const Center(child: CircularProgressIndicator())
          : ListView(
              children: [
                if (!account.loggedIn) _LoginCard(onLogin: _reload),
                if (_drafts.isNotEmpty) ...[
                  const _SectionHeader('本地草稿（未保存云端）'),
                  for (final d in _drafts)
                    ListTile(
                      leading: const Icon(Icons.edit_note),
                      title: Text(d.name),
                      subtitle: Text(
                        '${payloadTypeLabel(d.payloadType)} · ${d.items.length} 条',
                      ),
                      trailing: const Icon(Icons.chevron_right),
                      onTap: () => _openDraft(d),
                    ),
                ],
                if (account.loggedIn) ...[
                  const _SectionHeader('我的卡组（云端）'),
                  if (_loading)
                    const Padding(
                      padding: EdgeInsets.all(24),
                      child: Center(child: CircularProgressIndicator()),
                    )
                  else if (_error != null)
                    _HintCard(text: '加载失败：$_error')
                  else if (_decks.isEmpty)
                    const _HintCard(text: '暂无卡组：点「新建卡组」开始')
                  else
                    for (final d in _decks)
                      ListTile(
                        leading: const Icon(Icons.folder_special),
                        title: Text(d.name),
                        subtitle: Text(
                          '${payloadTypeLabel(d.payloadType)} · ${d.itemCount} 条',
                        ),
                        trailing: Text(
                          d.code,
                          style: const TextStyle(
                            fontSize: 12,
                            color: Colors.black45,
                          ),
                        ),
                        onTap: () => _openDetail(d),
                      ),
                ],
              ],
            ),
    );
  }

  Future<void> _showServerDialog(AccountController account) async {
    final ctrl = TextEditingController(text: account.baseUrl);
    await showDialog(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('云端服务器地址'),
        content: TextField(
          controller: ctrl,
          decoration: const InputDecoration(
            hintText: 'http://192.168.1.10:5228',
            border: OutlineInputBorder(),
          ),
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(ctx),
            child: const Text('取消'),
          ),
          FilledButton(
            onPressed: () async {
              await account.setBaseUrl(ctrl.text);
              if (ctx.mounted) Navigator.pop(ctx);
              await _reload();
            },
            child: const Text('保存'),
          ),
        ],
      ),
    );
  }
}

/// 版式模板中文标签（与 card_item_sheet 字段联动）
String payloadTypeLabel(String t) => switch (t) {
  'qa-card' => '问答卡',
  'poem-card' => '诗文卡',
  _ => '单词卡',
};

/// 登录/注册卡（内联切换）
class _LoginCard extends StatefulWidget {
  const _LoginCard({required this.onLogin});

  final Future<void> Function() onLogin;

  @override
  State<_LoginCard> createState() => _LoginCardState();
}

class _LoginCardState extends State<_LoginCard> {
  final _user = TextEditingController();
  final _pass = TextEditingController();
  final _name = TextEditingController();
  bool _register = false;
  bool _busy = false;

  @override
  void dispose() {
    _user.dispose();
    _pass.dispose();
    _name.dispose();
    super.dispose();
  }

  Future<void> _submit() async {
    final account = context.read<AccountController>();
    setState(() => _busy = true);
    try {
      if (_register) {
        await account.register(_user.text.trim(), _pass.text, _name.text);
      } else {
        await account.login(_user.text.trim(), _pass.text);
      }
      await widget.onLogin();
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(
          context,
        ).showSnackBar(SnackBar(content: Text('失败：$e')));
      }
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    return Card(
      margin: const EdgeInsets.all(12),
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Text(
              _register ? '注册账户' : '登录账户',
              style: Theme.of(context).textTheme.titleMedium,
            ),
            const SizedBox(height: 4),
            Text(
              '登录后卡组云端保存（换机可恢复）；不登录也可本地编辑并推送设备',
              style: const TextStyle(fontSize: 12, color: Colors.black54),
            ),
            const SizedBox(height: 12),
            TextField(
              controller: _user,
              decoration: const InputDecoration(
                labelText: '用户名',
                border: OutlineInputBorder(),
                isDense: true,
              ),
            ),
            const SizedBox(height: 8),
            TextField(
              controller: _pass,
              obscureText: true,
              decoration: const InputDecoration(
                labelText: '密码（≥6 位）',
                border: OutlineInputBorder(),
                isDense: true,
              ),
            ),
            if (_register) ...[
              const SizedBox(height: 8),
              TextField(
                controller: _name,
                decoration: const InputDecoration(
                  labelText: '昵称（可选）',
                  border: OutlineInputBorder(),
                  isDense: true,
                ),
              ),
            ],
            const SizedBox(height: 12),
            FilledButton(
              onPressed: _busy ? null : _submit,
              child: Text(_register ? '注册并登录' : '登录'),
            ),
            TextButton(
              onPressed: () => setState(() => _register = !_register),
              child: Text(_register ? '已有账户？去登录' : '没有账户？注册一个'),
            ),
          ],
        ),
      ),
    );
  }
}

class _SectionHeader extends StatelessWidget {
  const _SectionHeader(this.text);
  final String text;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 14, 16, 4),
      child: Text(
        text,
        style: Theme.of(
          context,
        ).textTheme.labelLarge?.copyWith(color: Colors.black54),
      ),
    );
  }
}

class _HintCard extends StatelessWidget {
  const _HintCard({required this.text});
  final String text;

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
