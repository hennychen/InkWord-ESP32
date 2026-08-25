/// 卡组编辑器数据编解码（v1.5 T5.3）：
///
/// - **CSV 导入**：Excel/WPS「另存为 CSV UTF-8」即可导入（零 xlsx 解析
///   依赖）。列序 = 正面,背面[,音标,例句]（word-card 全四列；qa/poem
///   前两列：题面/答案、上句/下句）。RFC 4180 子集：双引号包裹、""
///   转义、字段内逗号/换行。首行表头智能跳过（含「正面/front/题面/上句」）。
/// - **words.json 组装**：设备 word_parser 契约（顶层 {"words":[...]}，
///   字段 id/text/phonetic/meaning/example；qa/poem 走语义泛化——
///   text=题面/上句、meaning=答案/下句，T4.3 card_layout 按 manifest
///   payload_type 分派版式）。LAN 直传复用 T3.4 uploadDeck 全量覆盖
///   SD decks/{id}/words.json。
library;

import 'dart:convert';
import 'dart:typed_data';

/// 编辑器条目（与后端 ItemReq/ItemDto 同构）
class EditorItem {
  EditorItem({
    required this.front,
    required this.back,
    this.phonetic = '',
    this.example = '',
    this.id,
  });

  String front;
  String back;
  String phonetic;
  String example;

  /// 云端条目 Id（null = 新增未保存）
  String? id;

  EditorItem copy() => EditorItem(
    front: front,
    back: back,
    phonetic: phonetic,
    example: example,
    id: id,
  );
}

/// 解析 CSV 文本为条目列表（[maxRows] 防误选大文件，默认 2000 与
/// 后端单批上限一致）。空行跳过；正面留空的行跳过（脏数据容错）。
List<EditorItem> parseCsv(String text, {int maxRows = 2000}) {
  final rows = parseCsvRows(text);
  final items = <EditorItem>[];
  for (final row in rows) {
    if (items.length >= maxRows) break;
    if (row.isEmpty) continue;
    final front = row[0].trim();
    if (front.isEmpty) continue;
    final back = row.length > 1 ? row[1].trim() : '';
    items.add(
      EditorItem(
        front: front,
        back: back,
        phonetic: row.length > 2 ? row[2].trim() : '',
        example: row.length > 3 ? row[3].trim() : '',
      ),
    );
  }
  return items;
}

/// CSV 行解析（RFC 4180 子集状态机；返回按行分组的字段集合）。
/// 首行表头检测：任一字段命中「正面/front/题面/上句/question」即跳过。
List<List<String>> parseCsvRows(String text) {
  final rows = <List<String>>[];
  var field = StringBuffer();
  var row = <String>[];
  var inQuotes = false;
  var i = 0;
  // BOM 容错（Excel UTF-8 CSV 常带）
  if (text.startsWith('\uFEFF')) i = 1;

  void endField() {
    row.add(field.toString());
    field = StringBuffer();
  }

  void endRow() {
    endField();
    // 全空行丢弃（仅一个空字段的行）
    if (!(row.length == 1 && row[0].isEmpty)) rows.add(row);
    row = <String>[];
  }

  while (i < text.length) {
    final c = text[i];
    if (inQuotes) {
      if (c == '"') {
        if (i + 1 < text.length && text[i + 1] == '"') {
          field.write('"'); // "" 转义为字面引号
          i++;
        } else {
          inQuotes = false;
        }
      } else {
        field.write(c);
      }
    } else if (c == '"') {
      inQuotes = true;
    } else if (c == ',') {
      endField();
    } else if (c == '\r') {
      // 与 \n 合并处理（CRLF / 老 Mac CR 均按换行）
      if (i + 1 < text.length && text[i + 1] == '\n') i++;
      endRow();
    } else if (c == '\n') {
      endRow();
    } else {
      field.write(c);
    }
    i++;
  }
  if (field.isNotEmpty || row.isNotEmpty) endRow();

  // 表头跳过
  if (rows.isNotEmpty) {
    final head = rows.first.join(' ').toLowerCase();
    const marks = ['正面', 'front', '题面', '上句', 'question', 'word', '单词'];
    if (marks.any(head.contains)) rows.removeAt(0);
  }
  return rows;
}

/// 组装设备 words.json（LAN 直传 body）。
/// [payloadType] 仅用于注释说明（设备按 manifest 分派，body 内不携带）。
Uint8List buildWordsJson(
  List<EditorItem> items, {
  String payloadType = 'word-card',
}) {
  final words = <Map<String, dynamic>>[];
  var id = 0;
  for (final it in items) {
    if (it.front.trim().isEmpty) continue;
    id++;
    words.add({
      'id': id,
      'text': it.front.trim(),
      'phonetic': it.phonetic.trim(),
      'meaning': it.back.trim(),
      'example': it.example.trim(),
    });
  }
  final j = const JsonEncoder.withIndent(' ').convert({'words': words});
  return Uint8List.fromList(utf8.encode(j));
}
