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
    this.isShared = false,
    this.ownerName,
  });

  final String id;
  final String code; // 设备 deck id 同源（≤7 字符）
  final String name;
  final String payloadType;
  final String description;
  final String subjectCode;
  final int itemCount;

  /// v2.0 #3 UGC 分享：已公开到发现页（仅自己的卡组回填）
  final bool isShared;

  /// 分享者展示名（仅发现页条目回填；自己的卡组为 null）
  final String? ownerName;
}

/// 发现页一页结果（`GET /api/me/decks/shared`，v2.0 #3 UGC 分享）
class SharedDeckPage {
  const SharedDeckPage({required this.items, required this.hasMore});
  final List<CloudDeck> items;
  final bool hasMore; // 服务端 pageSize+1 探测
}

/// 账户绑定的设备（v2.0 完整账户，GET /api/me/devices）
class CloudDevice {
  const CloudDevice({
    required this.id,
    required this.name,
    required this.mac,
    this.firmwareVersion = '',
    this.batteryLevel = 100,
    this.online = false,
    this.lastHeartbeat,
    this.recordCount = 0,
  });

  final String id;
  final String name;
  final String mac; // 12 位大写 hex（与设备注册同源）
  final String firmwareVersion;
  final int batteryLevel;
  final bool online;
  final DateTime? lastHeartbeat;
  final int recordCount;
}

/// 跨设备 LWS 聚合行（GET /api/me/progress/aggregate，只读视图）
class CloudAggregate {
  const CloudAggregate({
    required this.wordId,
    required this.deviceId,
    required this.stability,
    required this.difficulty,
    this.isCollected = false,
    this.lastStudiedAt,
  });

  final String wordId;
  final String deviceId; // 胜出设备（LastStudiedAt 新者胜）
  final double stability;
  final double difficulty;
  final bool isCollected;
  final DateTime? lastStudiedAt;
}

/// LWS 聚合响应（P2 学习报告）：items 归并行 +
/// masteredCount 跨设备去重墨封词数（报告页头部统计）
class CloudAggregateSummary {
  const CloudAggregateSummary({
    required this.items,
    required this.masteredCount,
  });

  final List<CloudAggregate> items;
  final int masteredCount;
}

/// AI 对话周报一条（GET /api/me/devices/{id}/chat-review，P2 学习报告）。
/// 后端 review 为五段 JSON 对象（summary/topics/highlights/suggestion/
/// reviewWords），非法 JSON 兜底原文字符串进 summary。
class ChatReviewData {
  const ChatReviewData({
    required this.weekStart,
    required this.turnCount,
    this.summary = '',
    this.suggestion = '',
    this.topics = const [],
    this.highlights = const [],
    this.reviewWords = const [],
  });

  final DateTime? weekStart; // 周起始（周一 00:00 UTC）
  final int turnCount;
  final String summary;
  final String suggestion;
  final List<String> topics;
  final List<String> highlights;
  final List<String> reviewWords;

  factory ChatReviewData.fromMap(Map<String, dynamic> m) {
    final weekStart = DateTime.tryParse(m['weekStart'] as String? ?? '');
    final turnCount = (m['turnCount'] as num?)?.toInt() ?? 0;
    final r = m['review'];
    if (r is Map<String, dynamic>) {
      return ChatReviewData(
        weekStart: weekStart,
        turnCount: turnCount,
        summary: r['summary'] as String? ?? '',
        suggestion: r['suggestion'] as String? ?? '',
        topics: [for (final t in (r['topics'] as List? ?? [])) t.toString()],
        highlights: [
          for (final h in (r['highlights'] as List? ?? [])) h.toString(),
        ],
        reviewWords: [
          for (final w in (r['reviewWords'] as List? ?? [])) w.toString(),
        ],
      );
    }
    // 非法 JSON 兜底：review 原文字符串进 summary（属 404 空态外的降级）
    return ChatReviewData(
      weekStart: weekStart,
      turnCount: turnCount,
      summary: r?.toString() ?? '',
    );
  }
}

/// 单设备阅读记录行（GET /api/me/devices/{id}/reading，P2 学习报告）
class DeviceReading {
  const DeviceReading({
    required this.bookKey,
    required this.title,
    required this.currentPage,
    required this.totalPages,
    required this.progressPct,
    required this.totalReadMinutes,
    this.lastReadAt,
  });

  final String bookKey;
  final String title;
  final int currentPage;
  final int totalPages;
  final int progressPct; // 0-100
  final int totalReadMinutes; // 累计阅读分钟
  final DateTime? lastReadAt;
}

/// 云端书库条目（GET /api/me/books，与设备端 books 同 Published 口径）
class CloudBook {
  const CloudBook({
    required this.bookKey,
    required this.title,
    this.author = '',
    this.language = 'zh',
    this.fileSize = 0,
    this.format = 'txt',
    this.downloadCount = 0,
  });

  final String bookKey;
  final String title;
  final String author;
  final String language;
  final int fileSize; // 字节
  final String format; // txt / md / html
  final int downloadCount;
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
          isShared: e['isShared'] as bool? ?? false,
        ),
    ];
  }

  /// 发现页：他人已分享卡组（q 名称过滤 / subject 科目 Code / 分页）
  Future<SharedDeckPage> sharedDecks({
    int page = 1,
    int pageSize = 20,
    String? q,
    String? subject,
  }) async {
    final data = await _send(
      http.Request(
        'GET',
        _u(
          '/api/me/decks/shared'
          '?page=$page&pageSize=$pageSize'
          '${q == null || q.isEmpty ? "" : "&q=${Uri.encodeQueryComponent(q)}"}'
          '${subject == null || subject.isEmpty ? "" : "&subject=${Uri.encodeQueryComponent(subject)}"}',
        ),
      )..headers.addAll(_headers),
    );
    final m = (data as Map).cast<String, dynamic>();
    return SharedDeckPage(
      items: [
        for (final e in (m['items'] as List? ?? []))
          CloudDeck(
            id: e['id'] as String,
            code: '', // 发现页不回传 code（fork 后由副本携带）
            name: e['name'] as String? ?? '',
            payloadType: e['payloadType'] as String? ?? 'word-card',
            description: e['description'] as String? ?? '',
            subjectCode: e['subjectCode'] as String? ?? 'en',
            itemCount: e['itemCount'] as int? ?? 0,
            ownerName: e['ownerName'] as String? ?? '',
          ),
      ],
      hasMore: m['hasMore'] as bool? ?? false,
    );
  }

  /// 分享开关（仅自己的卡组；关闭不影响他人已导入副本）
  Future<void> setDeckShared(String deckId, bool shared) => _send(
    http.Request('POST', _u('/api/me/decks/$deckId/share'))
      ..headers.addAll(_headers)
      ..body = jsonEncode({'shared': shared}),
  );

  /// 导入（fork 深拷贝）：官方或他人已分享卡组 → 独立副本归自己。
  /// 走 `_send` 直发：响应含 int/bool 字段（itemCount/isShared），
  /// `_post` 内部 `cast<String,String>` 会炸（bindDevice 同陷阱先例）。
  Future<CloudDeck> forkDeck(String deckId, {String? name}) async {
    final data = await _send(
      http.Request('POST', _u('/api/me/decks/$deckId/fork'))
        ..headers.addAll(_headers)
        ..body = jsonEncode({'name': name}),
    );
    final m = (data as Map).cast<String, dynamic>();
    return CloudDeck(
      id: m['id'] as String,
      code: m['code'] as String? ?? '',
      name: m['name'] as String? ?? '',
      payloadType: m['payloadType'] as String? ?? 'word-card',
      description: m['description'] as String? ?? '',
      subjectCode: m['subjectCode'] as String? ?? 'en',
      itemCount: m['itemCount'] as int? ?? 0,
    );
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

  // ---- 我的设备（v2.0 完整账户：绑定/解绑/LWS 聚合） ----

  Future<List<CloudDevice>> myDevices() async {
    final data = await _send(
      http.Request('GET', _u('/api/me/devices'))..headers.addAll(_headers),
    );
    return [
      for (final e in (data as List? ?? []))
        CloudDevice(
          id: e['id'] as String,
          name: e['name'] as String? ?? '',
          mac: e['mac'] as String? ?? '',
          firmwareVersion: e['firmwareVersion'] as String? ?? '',
          batteryLevel: (e['batteryLevel'] as num?)?.toInt() ?? 0,
          online: e['online'] as bool? ?? false,
          lastHeartbeat: e['lastHeartbeat'] == null
              ? null
              : DateTime.tryParse(e['lastHeartbeat'] as String),
          recordCount: (e['recordCount'] as num?)?.toInt() ?? 0,
        ),
    ];
  }

  /// 绑定设备（凭 LAN /api/stats 的 mac）。成功后云端换发 ApiKey，
  /// 设备下个同步周期 401 自愈重注册取回新钥——无需 App 回送。
  /// 不走 _post：响应含 int/bool 字段，`_post` 的 `cast<String,String>` 会炸。
  Future<CloudDevice> bindDevice(String mac) async {
    final data = await _send(
      http.Request('POST', _u('/api/me/devices/bind'))
        ..headers.addAll(_headers)
        ..body = jsonEncode({'mac': mac}),
    );
    final m = (data as Map).cast<String, dynamic>();
    return CloudDevice(
      id: m['id'] as String,
      name: m['name'] as String? ?? '',
      mac: m['mac'] as String? ?? mac,
    );
  }

  Future<void> unbindDevice(String deviceId) => _send(
    http.Request('POST', _u('/api/me/devices/$deviceId/unbind'))
      ..headers.addAll(_headers),
  );

  /// LWS 聚合（P2 起响应含 masteredCount：跨设备去重墨封词数）。
  /// 旧后端/零设备早退分支可能回旧形态 List —— 按旧形态兼容做客户端兜底。
  Future<CloudAggregateSummary> aggregateProgress({int take = 500}) async {
    final data = await _send(
      http.Request('GET', _u('/api/me/progress/aggregate?take=$take'))
        ..headers.addAll(_headers),
    );
    // 兼容旧 List 形态（零设备空态）：视为空聚合而非类型错误
    if (data is List) {
      return const CloudAggregateSummary(items: [], masteredCount: 0);
    }
    final m = (data as Map).cast<String, dynamic>();
    return CloudAggregateSummary(
      masteredCount: (m['masteredCount'] as num?)?.toInt() ?? 0,
      items: [
        for (final e in (m['items'] as List? ?? []))
          CloudAggregate(
            wordId: e['wordId'] as String,
            deviceId: e['deviceId'] as String,
            stability: (e['stability'] as num?)?.toDouble() ?? 0,
            difficulty: (e['difficulty'] as num?)?.toDouble() ?? 0,
            isCollected: e['isCollected'] as bool? ?? false,
            lastStudiedAt: e['lastStudiedAt'] == null
                ? null
                : DateTime.tryParse(e['lastStudiedAt'] as String),
          ),
      ],
    );
  }

  // ---- P2 学习报告（2026-09）：周报 / 阅读 / 云端书库 ----

  /// AI 对话周报：按周倒序取最新 limit 条（默认 1，历史周切换分页拉）；
  /// 无周报（Job 未跑/无对话记录）抛 CloudException 404，调用方空态展示
  Future<List<ChatReviewData>> fetchChatReview(
    String deviceId, {
    int limit = 1,
  }) async {
    final data = await _send(
      http.Request(
        'GET',
        _u('/api/me/devices/$deviceId/chat-review?limit=$limit'),
      )..headers.addAll(_headers),
    );
    final m = (data as Map).cast<String, dynamic>();
    return [
      for (final e in (m['items'] as List? ?? []))
        ChatReviewData.fromMap((e as Map).cast<String, dynamic>()),
    ];
  }

  /// 设备阅读记录：ReadingProgress 按 LastReadAt 倒序
  Future<List<DeviceReading>> fetchDeviceReading(String deviceId) async {
    final data = await _send(
      http.Request('GET', _u('/api/me/devices/$deviceId/reading'))
        ..headers.addAll(_headers),
    );
    final m = (data as Map).cast<String, dynamic>();
    return [
      for (final e in (m['books'] as List? ?? []))
        DeviceReading(
          bookKey: e['bookKey'] as String? ?? '',
          title: e['title'] as String? ?? '',
          currentPage: (e['currentPage'] as num?)?.toInt() ?? 0,
          totalPages: (e['totalPages'] as num?)?.toInt() ?? 0,
          progressPct: (e['progressPct'] as num?)?.toInt() ?? 0,
          totalReadMinutes: (e['totalReadMinutes'] as num?)?.toInt() ?? 0,
          lastReadAt: e['lastReadAt'] == null
              ? null
              : DateTime.tryParse(e['lastReadAt'] as String),
        ),
    ];
  }

  /// 云端书库（只读浏览）：下载引导走设备端「我的书架→云端书架」
  Future<List<CloudBook>> cloudBooks() async {
    final data = await _send(
      http.Request('GET', _u('/api/me/books'))..headers.addAll(_headers),
    );
    return [
      for (final e in (data as List? ?? []))
        CloudBook(
          bookKey: e['bookKey'] as String,
          title: e['title'] as String? ?? '',
          author: e['author'] as String? ?? '',
          language: e['language'] as String? ?? 'zh',
          fileSize: (e['fileSize'] as num?)?.toInt() ?? 0,
          format: e['format'] as String? ?? 'txt',
          downloadCount: (e['downloadCount'] as num?)?.toInt() ?? 0,
        ),
    ];
  }
}
