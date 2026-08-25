/// 卡组编辑器编解码测试（v1.5 T5.3）
library;

import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:inkword_app/features/editor/deck_codec.dart';

void main() {
  group('parseCsvRows', () {
    test('基本逗号分隔 + CRLF + 尾行无换行', () {
      final rows = parseCsvRows('a,b,c\r\nx,y\r\nlast');
      expect(rows, [
        ['a', 'b', 'c'],
        ['x', 'y'],
        ['last'],
      ]);
    });

    test('引号包裹：字段内逗号/换行/"" 转义', () {
      final rows = parseCsvRows('"a,1",b\n"line1\nline2","q""uote"');
      expect(rows, [
        ['a,1', 'b'],
        ['line1\nline2', 'q"uote'],
      ]);
    });

    test('BOM 容错 + 空行丢弃', () {
      final rows = parseCsvRows('\uFEFFhello,hi\n\n\nworld,x');
      expect(rows, [
        ['hello', 'hi'],
        ['world', 'x'],
      ]);
    });

    test('表头智能跳过（中文/英文标记）', () {
      expect(parseCsvRows('单词,释义\napple,苹果').length, 1);
      expect(parseCsvRows('front,back\napple,苹果').length, 1);
      expect(parseCsvRows('题面,答案\nQ,A').length, 1);
      // 无表头标记不误跳
      expect(parseCsvRows('apple,苹果').length, 1);
    });
  });

  group('parseCsv', () {
    test('四列 word-card + 脏数据容错', () {
      final items = parseCsv(
        'word,meaning,phonetic,example\napple,苹果,/æpl/,an apple\n,空正面跳过\nbook,书',
      );
      expect(items.length, 2);
      expect(items[0].front, 'apple');
      expect(items[0].phonetic, '/æpl/');
      expect(items[0].example, 'an apple');
      expect(items[1].back, '书');
      expect(items[1].phonetic, '');
    });

    test('maxRows 截断', () {
      final text = List.filled(10, 'w,b').join('\n');
      expect(parseCsv(text, maxRows: 5).length, 5);
    });
  });

  group('buildWordsJson', () {
    test('设备契约：{"words":[{id,text,phonetic,meaning,example}]}', () {
      final bytes = buildWordsJson([
        EditorItem(front: 'apple', back: '苹果', phonetic: '/æpl/', example: 'x'),
        EditorItem(front: '床前明月光', back: '疑是地上霜'),
      ]);
      final j = jsonDecode(utf8.decode(bytes)) as Map<String, dynamic>;
      final words = j['words'] as List;
      expect(words.length, 2);
      expect(words[0]['id'], 1);
      expect(words[0]['text'], 'apple');
      expect(words[0]['meaning'], '苹果');
      // poem 卡语义泛化：text=上句 / meaning=下句
      expect(words[1]['text'], '床前明月光');
      expect(words[1]['meaning'], '疑是地上霜');
      expect(words[1]['phonetic'], '');
    });

    test('空正面条目跳过 + id 重排连续', () {
      final bytes = buildWordsJson([
        EditorItem(front: '  ', back: 'x'), // 脏数据
        EditorItem(front: 'a', back: '1'),
        EditorItem(front: 'b', back: '2'),
      ]);
      final words = (jsonDecode(utf8.decode(bytes)) as Map)['words'] as List;
      expect(words.length, 2);
      expect(words[0]['id'], 1);
      expect(words[1]['id'], 2);
    });
  });
}
