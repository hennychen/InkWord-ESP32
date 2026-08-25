/// 条目编辑表单（v1.5 T5.3）：按卡组版式动态字段
///
/// - word-card：单词* / 音标 / 释义* / 例句
/// - qa-card：题面* / 答案*
/// - poem-card：上句* / 下句*
///
/// 保存即返回 EditorItem（id 保留原值，null=新增）；取消返回 null。
library;

import 'package:flutter/material.dart';

import 'deck_codec.dart';

Future<EditorItem?> showCardItemSheet(
  BuildContext context, {
  required String payloadType,
  EditorItem? initial,
}) {
  return showModalBottomSheet<EditorItem>(
    context: context,
    isScrollControlled: true,
    builder: (_) => _CardItemSheet(payloadType: payloadType, initial: initial),
  );
}

class _CardItemSheet extends StatefulWidget {
  const _CardItemSheet({required this.payloadType, this.initial});

  final String payloadType;
  final EditorItem? initial;

  @override
  State<_CardItemSheet> createState() => _CardItemSheetState();
}

class _CardItemSheetState extends State<_CardItemSheet> {
  late final TextEditingController _front;
  late final TextEditingController _back;
  late final TextEditingController _phonetic;
  late final TextEditingController _example;

  @override
  void initState() {
    super.initState();
    final it = widget.initial;
    _front = TextEditingController(text: it?.front ?? '');
    _back = TextEditingController(text: it?.back ?? '');
    _phonetic = TextEditingController(text: it?.phonetic ?? '');
    _example = TextEditingController(text: it?.example ?? '');
  }

  @override
  void dispose() {
    _front.dispose();
    _back.dispose();
    _phonetic.dispose();
    _example.dispose();
    super.dispose();
  }

  String get _frontLabel => switch (widget.payloadType) {
    'qa-card' => '题面',
    'poem-card' => '上句',
    _ => '单词',
  };

  String get _backLabel => switch (widget.payloadType) {
    'qa-card' => '答案',
    'poem-card' => '下句',
    _ => '释义',
  };

  void _save() {
    if (_front.text.trim().isEmpty || _back.text.trim().isEmpty) return;
    Navigator.pop(
      context,
      EditorItem(
        id: widget.initial?.id,
        front: _front.text.trim(),
        back: _back.text.trim(),
        phonetic: widget.payloadType == 'word-card'
            ? _phonetic.text.trim()
            : '',
        example: widget.payloadType == 'word-card' ? _example.text.trim() : '',
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final isWord = widget.payloadType == 'word-card';
    return Padding(
      padding: EdgeInsets.only(
        left: 16,
        right: 16,
        top: 16,
        // 键盘弹起避让
        bottom: MediaQuery.of(context).viewInsets.bottom + 16,
      ),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Text(
            widget.initial == null ? '新增条目' : '编辑条目',
            style: Theme.of(context).textTheme.titleMedium,
          ),
          const SizedBox(height: 12),
          TextField(
            controller: _front,
            maxLines: isWord ? 1 : 2,
            decoration: InputDecoration(
              labelText: '$_frontLabel（必填）',
              border: const OutlineInputBorder(),
            ),
          ),
          const SizedBox(height: 10),
          TextField(
            controller: _back,
            maxLines: isWord ? 2 : 3,
            decoration: InputDecoration(
              labelText: '$_backLabel（必填）',
              border: const OutlineInputBorder(),
            ),
          ),
          if (isWord) ...[
            const SizedBox(height: 10),
            TextField(
              controller: _phonetic,
              decoration: const InputDecoration(
                labelText: '音标（可选，如 /həˈləʊ/）',
                border: OutlineInputBorder(),
              ),
            ),
            const SizedBox(height: 10),
            TextField(
              controller: _example,
              maxLines: 2,
              decoration: const InputDecoration(
                labelText: '例句（可选）',
                border: OutlineInputBorder(),
              ),
            ),
          ],
          const SizedBox(height: 14),
          FilledButton.icon(
            onPressed: _save,
            icon: const Icon(Icons.check),
            label: const Text('保存'),
          ),
        ],
      ),
    );
  }
}
