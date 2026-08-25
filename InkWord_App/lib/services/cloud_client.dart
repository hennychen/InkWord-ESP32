/// 云端 API 客户端（v1.5 T5.3 轻账户 + 我的卡组）
///
/// 与设备直连 DeviceHttpClient 分离：本客户端走后端（InkWord.API），
/// learner JWT 鉴权（Authorization: Bearer）。baseUrl 家长侧可配
/// （默认 http://localhost:5228 开发实例），token/账户名持久化见
/// AccountController。离线优先红线（ACCOUNT_MODEL_DECISION §三.1）：
/// 云端不可达不影响设备直连全部功能，编辑器未登录仍可 LAN 推送。
library;

import 'dart:convert';

import 'package:http/http.dart' as http;

import '../features/editor/deck_codec.dart';

class CloudException implements Exception {
  CloudException(this.code, this.message);
  final int code;
  final String message;

  @override
  String toString() => message;
}

class CloudSubject {
  const CloudSubject({required this.code, required this.name});
  final String code;
  final String name;
}

class CloudDeck {
  const CloudDeck({
    required this.id,
    required this.code,
    required this.name,
    required this.payloadType,
    this.description = '',
    this.subjectCode = 'en',
    this.itemCount = 0,
  });

  final String id;
  final String code; // 设备 deck id 同源（≤7 字符）
  final String name;
  final String payloadType;
  final String description;
  final String subjectCode;
  final int itemCount;
}

class CloudClient {
  CloudClient({required this.baseUrl, this.token});

  final String baseUrl;
  String? token;
  final http.Client _http = http.Client();

  void close() => _http.close();

  Uri _u(String path) => Uri.parse('$baseUrl$path');

  Map<String, String> get _headers => {
    'Content-Type': 'application/json',
    if (token != null) 'Authorization': 'Bearer $token',
  };

  Future<dynamic> _send(http.BaseRequest req) async {
    http.Response resp;
    try {
      resp = await _http.send(req).then(http.Response.fromStream);
    } catch (e) {
      throw CloudException(-1, '无法连接服务器（$e）');
    }
    dynamic body;
    if (resp.body.isNotEmpty) {
      try {
        body = jsonDecode(utf8.decode(resp.bodyBytes));
      } catch (_) {
        body = null;
      }
    }
    final code = body is Map<String, dynamic> ? body['code'] as int? : null;
    if (resp.statusCode >= 400 || (code != null && code != 0)) {
      throw CloudException(
        code ?? resp.statusCode,
        body is Map<String, dynamic> && body['message'] != null
            ? body['message'].toString()
            : 'HTTP ${resp.statusCode}',
      );
    }
    return body is Map<String, dynamic> ? body['data'] : null;
  }

  Future<Map<String, String>> _post(
    String path,
    Map<String, dynamic> body,
  ) async {
    final data = await _send(
      http.Request('POST', _u(path))
        ..headers.addAll(_headers)
        ..body = jsonEncode(body),
    );
    return (data as Map).cast<String, String>();
  }

  // ---- 账户 ----

  /// 注册即登录；返回 {username, displayName, token}
  Future<Map<String, String>> register(
    String username,
    String password,
    String? displayName,
  ) async {
    final r = await _post('/api/account/register', {
      'username': username,
      'password': password,
      if (displayName != null && displayName.trim().isNotEmpty)
        'displayName': displayName.trim(),
    });
    token = r['token'];
    return r;
  }

  Future<Map<String, String>> login(String username, String password) async {
    final r = await _post('/api/account/login', {
      'username': username,
      'password': password,
    });
    token = r['token'];
    return r;
  }

  /// 校验本地 token（App 启动时；401 抛 CloudException）
  Future<Map<String, String>> me() async {
    final data = await _send(
      http.Request('GET', _u('/api/account/me'))..headers.addAll(_headers),
    );
    return (data as Map).cast<String, String>();
  }

  // ---- 我的卡组 ----

  Future<List<CloudDeck>> myDecks() async {
    final data = await _send(
      http.Request('GET', _u('/api/me/decks'))..headers.addAll(_headers),
    );
    return [
      for (final e in (data as List? ?? []))
        CloudDeck(
          id: e['id'] as String,
          code: e['code'] as String? ?? '',
          name: e['name'] as String? ?? '',
          payloadType: e['payloadType'] as String? ?? 'word-card',
          description: e['description'] as String? ?? '',
          subjectCode: e['subjectCode'] as String? ?? 'en',
          itemCount: e['itemCount'] as int? ?? 0,
        ),
    ];
  }

  Future<List<CloudSubject>> subjects() async {
    final data = await _send(
      http.Request('GET', _u('/api/me/subjects'))..headers.addAll(_headers),
    );
    return [
      for (final e in (data as List? ?? []))
        CloudSubject(
          code: e['code'] as String,
          name: e['name'] as String? ?? e['code'],
        ),
    ];
  }

  Future<CloudDeck> createDeck({
    required String subjectCode,
    required String name,
    required String payloadType,
    String description = '',
    List<EditorItem> items = const [],
  }) async {
    final data = await _post('/api/me/decks', {
      'subjectCode': subjectCode,
      'name': name,
      'payloadType': payloadType,
      'description': description,
      'items': [
        for (final it in items)
          {
            'front': it.front,
            'back': it.back,
            'phonetic': it.phonetic,
            'example': it.example,
          },
      ],
    });
    final m = (data as Map).cast<String, dynamic>();
    return CloudDeck(
      id: m['id'] as String,
      code: m['code'] as String? ?? '',
      name: m['name'] as String? ?? name,
      payloadType: m['payloadType'] as String? ?? payloadType,
      description: m['description'] as String? ?? description,
      subjectCode: subjectCode,
      itemCount: items.length,
    );
  }

  Future<List<EditorItem>> fetchItems(String deckId) async {
    final data = await _send(
      http.Request('GET', _u('/api/me/decks/$deckId/items'))
        ..headers.addAll(_headers),
    );
    return [
      for (final e in (data as List? ?? []))
        EditorItem(
          id: e['id'] as String,
          front: e['front'] as String? ?? '',
          back: e['back'] as String? ?? '',
          phonetic: e['phonetic'] as String? ?? '',
          example: e['example'] as String? ?? '',
        ),
    ];
  }

  Future<void> updateDeck(String deckId, String name, String? description) =>
      _send(
        http.Request('PUT', _u('/api/me/decks/$deckId'))
          ..headers.addAll(_headers)
          ..body = jsonEncode({'name': name, 'description': description}),
      );

  Future<void> deleteDeck(String deckId) => _send(
    http.Request('DELETE', _u('/api/me/decks/$deckId'))
      ..headers.addAll(_headers),
  );

  Future<void> addItems(String deckId, List<EditorItem> items) => _send(
    http.Request('POST', _u('/api/me/decks/$deckId/items'))
      ..headers.addAll(_headers)
      ..body = jsonEncode({
        'items': [
          for (final it in items)
            {
              'front': it.front,
              'back': it.back,
              'phonetic': it.phonetic,
              'example': it.example,
            },
        ],
      }),
  );

  Future<void> updateItem(String deckId, EditorItem it) => _send(
    http.Request('PUT', _u('/api/me/decks/$deckId/items/${it.id}'))
      ..headers.addAll(_headers)
      ..body = jsonEncode({
        'front': it.front,
        'back': it.back,
        'phonetic': it.phonetic,
        'example': it.example,
      }),
  );

  Future<void> deleteItem(String deckId, String itemId) => _send(
    http.Request('DELETE', _u('/api/me/decks/$deckId/items/$itemId'))
      ..headers.addAll(_headers),
  );
}
