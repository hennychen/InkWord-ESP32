/// 学习报告页（P2，2026-09）：本地统计 + 云端报告双分区
///
/// 本地区（v1.3 T3.4 原视图保留）：设备 GET /api/stats 直读
/// （lr_stats 口径：今日首评 / 评分总次数 / 连续天数 + 错词 / 到期 /
/// 收藏数），局域网直读无需云端。
/// 云端区（登录态可见）：AI 对话周报（chat-review，历史周切换）+
/// LWS 聚合进度（含墨封 masteredCount）+ 设备阅读记录 + 云端书库
/// 只读浏览（下载引导走设备端「我的书架→云端书架」自拉）。
/// 离线优先红线：云端不可达/未登录不影响本地区展示。
library;

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../../services/cloud_client.dart';
import '../../services/http_device_client.dart';
import '../../state/account_controller.dart';
import '../../state/device_controller.dart';

class StatsPage extends StatefulWidget {
  const StatsPage({super.key});

  @override
  State<StatsPage> createState() => _StatsPageState();
}

class _StatsPageState extends State<StatsPage> {
  // ---- 本地统计 ----
  DeviceStats? _stats;
  String? _error;
  bool _loading = true;

  // ---- 云端报告 ----
  AccountController? _accountRef;
  List<CloudDevice>? _devices;
  String? _selectedDeviceId;

  List<ChatReviewData> _reviews = [];
  int _reviewIndex = 0; // 0 = 最新一周
  int _reviewLimit = 1; // 已拉取周数（历史切换「加载更早」递增）
  bool _reviewEmpty = false; // 404：Job 未跑/无对话记录
  String? _reviewError;

  CloudAggregateSummary? _aggregate;
  List<DeviceReading> _readings = [];
  List<CloudBook> _books = [];
  bool _booksExpanded = false; // 书库卡展开全部（默认只显前 8 本）
  bool _cloudLoading = false;

  DeviceHttpClient? get _client => context.read<DeviceController>().client;

  CloudClient? get _cloud => _accountRef?.client;

  @override
  void initState() {
    super.initState();
    _reloadLocal();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted) return;
      final account = context.read<AccountController>();
      _accountRef = account;
      account.addListener(_onAccountChanged);
      _onAccountChanged();
    });
  }

  @override
  void dispose() {
    _accountRef?.removeListener(_onAccountChanged);
    super.dispose();
  }

  /// 登录/登出联动：登录拉云端报告，登出清空（本地区不受影响）
  void _onAccountChanged() {
    if (!mounted) return;
    if (_accountRef!.loggedIn) {
      _reloadCloud();
    } else {
      setState(() {
        _devices = null;
        _selectedDeviceId = null;
        _reviews = [];
        _reviewEmpty = false;
        _reviewError = null;
        _aggregate = null;
        _readings = [];
        _books = [];
        _booksExpanded = false;
      });
    }
  }

  Future<void> _reloadLocal() async {
    final c = _client;
    if (c == null) {
      if (mounted) setState(() => _loading = false);
      return;
    }
    setState(() => _loading = true);
    try {
      final st = await c.fetchStats();
      if (!mounted) return;
      setState(() {
        _stats = st;
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

  /// 云端总拉取：设备清单 + 账户级聚合 + 云端书库（不依赖设备选择）
  Future<void> _reloadCloud() async {
    final cloud = _cloud;
    if (cloud == null) return;
    setState(() => _cloudLoading = true);

    // 账户级数据（与设备无关）并行拉
    cloud
        .aggregateProgress()
        .then((agg) {
          if (mounted) setState(() => _aggregate = agg);
        })
        .catchError((_) {});
    cloud
        .cloudBooks()
        .then((books) {
          if (mounted) setState(() => _books = books);
        })
        .catchError((_) {});

    try {
      final devices = await cloud.myDevices();
      if (!mounted) return;
      setState(() {
        _devices = devices;
        _cloudLoading = false;
        // 选中设备失效（解绑后）则回落首个
        if (_selectedDeviceId == null ||
            !devices.any((d) => d.id == _selectedDeviceId)) {
          _selectedDeviceId = devices.isEmpty ? null : devices.first.id;
          _reviewIndex = 0;
        }
      });
      // 周报/阅读随设备清单一并刷新（下拉刷新对已选设备同样生效）
      if (_selectedDeviceId != null) _loadDeviceReport();
    } catch (e) {
      if (!mounted) return;
      setState(() => _cloudLoading = false);
    }
  }

  /// 选中设备的周报 + 阅读记录
  Future<void> _loadDeviceReport() async {
    final cloud = _cloud;
    final id = _selectedDeviceId;
    if (cloud == null || id == null) return;

    setState(() {
      _reviewError = null;
      _reviewEmpty = false;
    });

    cloud
        .fetchChatReview(id, limit: _reviewLimit)
        .then((reviews) {
          if (!mounted) return;
          setState(() {
            _reviews = reviews;
            if (_reviewIndex >= reviews.length) {
              _reviewIndex = reviews.length - 1;
            }
          });
        })
        .catchError((e) {
          if (!mounted) return;
          setState(() {
            _reviews = [];
            if (e is CloudException && e.code == 404) {
              _reviewEmpty = true; // 周日 Job 未跑 / 无对话记录
            } else {
              _reviewError = e.toString();
            }
          });
        });

    cloud
        .fetchDeviceReading(id)
        .then((readings) {
          if (mounted) setState(() => _readings = readings);
        })
        .catchError((_) {
          if (mounted) setState(() => _readings = []);
        });
  }

  void _onDeviceSelected(String? id) {
    if (id == null || id == _selectedDeviceId) return;
    setState(() {
      _selectedDeviceId = id;
      _reviewIndex = 0;
      _reviewLimit = 1;
    });
    _loadDeviceReport();
  }

  /// 历史周切换：前/后一周（0 = 最新）
  void _shiftReview(int delta) {
    final next = _reviewIndex + delta;
    if (next < 0 || next >= _reviews.length) return;
    setState(() => _reviewIndex = next);
  }

  /// 加载更早周报（一次多拉 4 周；后端上限 26）
  void _loadOlderReviews() {
    _reviewLimit = (_reviewLimit + 4).clamp(1, 26);
    _loadDeviceReport();
  }

  Future<void> _reloadAll() async {
    await Future.wait([_reloadLocal(), _reloadCloud()]);
  }

  @override
  Widget build(BuildContext context) {
    final connected = context.watch<DeviceController>().connected;
    final loggedIn = context.watch<AccountController>().loggedIn;
    return Scaffold(
      appBar: AppBar(title: const Text('学习报告')),
      body: RefreshIndicator(
        onRefresh: _reloadAll,
        child: ListView(
          physics: const AlwaysScrollableScrollPhysics(),
          padding: const EdgeInsets.all(12),
          children: [
            _LocalSection(
              connected: connected,
              loading: _loading,
              error: _error,
              stats: _stats,
              onReload: _reloadLocal,
            ),
            const SizedBox(height: 12),
            if (loggedIn)
              _CloudSection(
                state: this,
                onToggleBooks: () =>
                    setState(() => _booksExpanded = !_booksExpanded),
              )
            else
              _loginHintCard(context),
          ],
        ),
      ),
    );
  }

  Widget _loginHintCard(BuildContext context) {
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Icon(
                  Icons.cloud_outlined,
                  color: Theme.of(context).colorScheme.primary,
                ),
                const SizedBox(width: 10),
                const Text(
                  '云端报告',
                  style: TextStyle(fontSize: 16, fontWeight: FontWeight.w600),
                ),
              ],
            ),
            const SizedBox(height: 8),
            const Text(
              '登录后可查看 AI 对话周报、跨设备学习进度（含墨封）、'
              '阅读记录与云端书库。\n入口：词书页 → 卡组编辑器内登录。',
              style: TextStyle(color: Colors.black54, fontSize: 13),
            ),
          ],
        ),
      ),
    );
  }
}

// ==================== 本地统计区（v1.3 原视图） ====================

class _LocalSection extends StatelessWidget {
  final bool connected;
  final bool loading;
  final String? error;
  final DeviceStats? stats;
  final Future<void> Function() onReload;

  const _LocalSection({
    required this.connected,
    required this.loading,
    required this.error,
    required this.stats,
    required this.onReload,
  });

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        const _SectionTitle(icon: Icons.bar_chart, text: '本地统计（设备直读）'),
        if (!connected)
          Card(
            child: Padding(
              padding: const EdgeInsets.all(14),
              child: Text(
                '未连接设备：请先在「设备」页连接（BLE / mDNS / IP）',
                style: TextStyle(color: Colors.black54),
              ),
            ),
          )
        else if (loading)
          const Card(
            child: Padding(
              padding: EdgeInsets.all(24),
              child: Center(child: CircularProgressIndicator()),
            ),
          )
        else if (error != null)
          Card(
            child: Padding(
              padding: const EdgeInsets.all(14),
              child: Text('读取失败：$error'),
            ),
          )
        else if (stats != null)
          _StatsView(stats: stats!),
      ],
    );
  }
}

class _StatsView extends StatelessWidget {
  final DeviceStats stats;
  const _StatsView({required this.stats});

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    return Column(
      children: [
        // 连续天数（主视觉：火焰 + 大数字）
        Card(
          child: Padding(
            padding: const EdgeInsets.all(18),
            child: Row(
              children: [
                Icon(
                  Icons.local_fire_department,
                  size: 44,
                  color: scheme.primary,
                ),
                const SizedBox(width: 14),
                Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      '${stats.streakDays} 天',
                      style: const TextStyle(
                        fontSize: 30,
                        fontWeight: FontWeight.bold,
                      ),
                    ),
                    const Text('连续学习', style: TextStyle(color: Colors.black54)),
                  ],
                ),
                const Spacer(),
                Column(
                  crossAxisAlignment: CrossAxisAlignment.end,
                  children: [
                    const Text('当前词书'),
                    Text(
                      stats.activeDeck,
                      style: const TextStyle(fontWeight: FontWeight.w600),
                    ),
                  ],
                ),
              ],
            ),
          ),
        ),
        const SizedBox(height: 12),

        // 今日统计行
        Row(
          children: [
            Expanded(
              child: _StatCard(
                icon: Icons.fiber_new,
                label: '今日新学',
                value: '${stats.todayNew}',
              ),
            ),
            const SizedBox(width: 12),
            Expanded(
              child: _StatCard(
                icon: Icons.replay,
                label: '今日复习',
                value: '${stats.todayReviews}',
              ),
            ),
          ],
        ),
        const SizedBox(height: 12),

        // 学习池概况
        Row(
          children: [
            Expanded(
              child: _StatCard(
                icon: Icons.schedule,
                label: '待复习',
                value: '${stats.dueCount}',
              ),
            ),
            const SizedBox(width: 12),
            Expanded(
              child: _StatCard(
                icon: Icons.error_outline,
                label: '错词本',
                value: '${stats.wrongCount}',
              ),
            ),
            const SizedBox(width: 12),
            Expanded(
              child: _StatCard(
                icon: Icons.star_border,
                label: '收藏',
                value: '${stats.collectedCount}',
              ),
            ),
          ],
        ),
        const SizedBox(height: 12),

        Card(
          child: ListTile(
            leading: const Icon(Icons.menu_book),
            title: const Text('词库总量'),
            trailing: Text(
              '${stats.totalWords} 词',
              style: const TextStyle(fontSize: 16, fontWeight: FontWeight.w600),
            ),
          ),
        ),

        const SizedBox(height: 8),
        const Padding(
          padding: EdgeInsets.symmetric(horizontal: 8),
          child: Text(
            '口径：设备端本地统计（lr_stats），跨词书累计；'
            '今日 = 首次评分计新学、评分总次数计复习。',
            style: TextStyle(fontSize: 11, color: Colors.black45),
          ),
        ),
      ],
    );
  }
}

// ==================== 云端报告区（P2，登录态可见） ====================

/// 云端报告区：直接读宿主 State 的字段（同文件私有意图，
/// 避免为一次性视图传递十余个参数）
class _CloudSection extends StatelessWidget {
  final _StatsPageState state;
  // 书库展开/收起需宿主 setState（StatelessWidget 无自身 State，
  // 跨类调 state.setState 属 protected 违规，故经回调注入）
  final VoidCallback onToggleBooks;
  const _CloudSection({required this.state, required this.onToggleBooks});

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        const _SectionTitle(icon: Icons.cloud_outlined, text: '云端报告'),
        _devicePicker(context),
        const SizedBox(height: 8),

        // 周报卡
        _reviewCard(context, scheme),

        const SizedBox(height: 12),

        // 云端聚合卡（账户级，不依赖设备选择）
        _aggregateCard(context),

        const SizedBox(height: 12),

        // 阅读记录卡
        _readingCard(context, scheme),

        const SizedBox(height: 12),

        // 云端书库卡
        _libraryCard(context),
      ],
    );
  }

  Widget _devicePicker(BuildContext context) {
    final devices = state._devices;
    if (state._cloudLoading && devices == null) {
      return const Card(
        child: Padding(
          padding: EdgeInsets.all(16),
          child: Center(child: CircularProgressIndicator()),
        ),
      );
    }
    if (devices == null || devices.isEmpty) {
      return Card(
        child: Padding(
          padding: const EdgeInsets.all(14),
          child: Text(
            '尚无绑定设备：请到「设备」页登录并绑定'
            '（LAN 发现一键绑定）',
            style: TextStyle(color: Colors.black54, fontSize: 13),
          ),
        ),
      );
    }
    return Card(
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 4),
        child: DropdownButtonFormField<String>(
          value: state._selectedDeviceId,
          decoration: const InputDecoration(
            icon: Icon(Icons.devices_other),
            border: InputBorder.none,
            labelText: '报告设备',
          ),
          items: [
            for (final d in devices)
              DropdownMenuItem(
                value: d.id,
                child: Text(
                  '${d.name.isEmpty ? d.mac : d.name}'
                  '${d.online ? "（在线）" : ""}',
                  overflow: TextOverflow.ellipsis,
                ),
              ),
          ],
          onChanged: state._onDeviceSelected,
        ),
      ),
    );
  }

  Widget _reviewCard(BuildContext context, ColorScheme scheme) {
    // 空态三分支：未选设备 / 404 无周报 / 拉取失败
    if (state._selectedDeviceId == null) {
      return const SizedBox.shrink();
    }
    if (state._reviewError != null) {
      return Card(
        child: Padding(
          padding: const EdgeInsets.all(14),
          child: Text(
            '周报拉取失败：${state._reviewError}',
            style: const TextStyle(color: Colors.black54, fontSize: 13),
          ),
        ),
      );
    }
    if (state._reviewEmpty || state._reviews.isEmpty) {
      return Card(
        child: Padding(
          padding: const EdgeInsets.all(14),
          child: Text(
            '暂无 AI 对话周报：周报每周日自动生成，'
            '本周与 AI 学伴对话一轮后下周可看。',
            style: TextStyle(color: Colors.black54, fontSize: 13),
          ),
        ),
      );
    }

    final r = state._reviews[state._reviewIndex];
    final fmt = r.weekStart == null
        ? ''
        : '${r.weekStart!.month}/${r.weekStart!.day} 当周';
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(14),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Icon(Icons.auto_awesome, color: scheme.primary, size: 20),
                const SizedBox(width: 8),
                const Expanded(
                  child: Text(
                    'AI 对话周报',
                    style: TextStyle(fontSize: 15, fontWeight: FontWeight.w600),
                  ),
                ),
                // 历史周切换
                IconButton(
                  visualDensity: VisualDensity.compact,
                  icon: const Icon(Icons.chevron_left),
                  tooltip: '后一周',
                  onPressed: state._reviewIndex > 0
                      ? () => state._shiftReview(-1)
                      : null,
                ),
                Text(fmt, style: const TextStyle(fontSize: 12)),
                IconButton(
                  visualDensity: VisualDensity.compact,
                  icon: const Icon(Icons.chevron_right),
                  tooltip: '前一周（更早）',
                  onPressed: state._reviewIndex < state._reviews.length - 1
                      ? () => state._shiftReview(1)
                      : null,
                ),
              ],
            ),
            const SizedBox(height: 4),
            Text(
              '本周对话 ${r.turnCount} 轮',
              style: const TextStyle(fontSize: 12, color: Colors.black45),
            ),
            if (r.summary.isNotEmpty) ...[
              const SizedBox(height: 8),
              Text(r.summary, style: const TextStyle(fontSize: 14)),
            ],
            if (r.topics.isNotEmpty) ...[
              const SizedBox(height: 8),
              Wrap(
                spacing: 6,
                runSpacing: 4,
                children: [
                  for (final t in r.topics)
                    Chip(
                      label: Text(t, style: const TextStyle(fontSize: 12)),
                      visualDensity: VisualDensity.compact,
                    ),
                ],
              ),
            ],
            if (r.suggestion.isNotEmpty) ...[
              const SizedBox(height: 8),
              Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Icon(
                    Icons.tips_and_updates_outlined,
                    size: 16,
                    color: Colors.black54,
                  ),
                  const SizedBox(width: 6),
                  Expanded(
                    child: Text(
                      r.suggestion,
                      style: const TextStyle(
                        fontSize: 13,
                        color: Colors.black87,
                      ),
                    ),
                  ),
                ],
              ),
            ],
            if (r.reviewWords.isNotEmpty) ...[
              const SizedBox(height: 8),
              Wrap(
                spacing: 6,
                runSpacing: 4,
                children: [
                  for (final w in r.reviewWords)
                    Chip(
                      label: Text(w, style: const TextStyle(fontSize: 12)),
                      visualDensity: VisualDensity.compact,
                      backgroundColor: Colors.black12,
                    ),
                ],
              ),
            ],
            // 已到已拉取周数边界且未到上限：加载更早
            if (state._reviewIndex == state._reviews.length - 1 &&
                state._reviewLimit < 26) ...[
              const SizedBox(height: 4),
              Align(
                alignment: Alignment.center,
                child: TextButton.icon(
                  onPressed: state._loadOlderReviews,
                  icon: const Icon(Icons.history, size: 16),
                  label: const Text('加载更早周报'),
                ),
              ),
            ],
          ],
        ),
      ),
    );
  }

  Widget _aggregateCard(BuildContext context) {
    final agg = state._aggregate;
    final collected = agg?.items.where((a) => a.isCollected).length ?? 0;
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(14),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Icon(
                  Icons.workspace_premium_outlined,
                  color: Theme.of(context).colorScheme.primary,
                  size: 20,
                ),
                const SizedBox(width: 8),
                const Text(
                  '跨设备学习进度',
                  style: TextStyle(fontSize: 15, fontWeight: FontWeight.w600),
                ),
              ],
            ),
            const SizedBox(height: 12),
            Row(
              children: [
                Expanded(
                  child: _StatCard(
                    icon: Icons.approval,
                    label: '墨封词数',
                    value: '${agg?.masteredCount ?? "-"}',
                  ),
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: _StatCard(
                    icon: Icons.school_outlined,
                    label: '学习词数',
                    value: agg == null ? '-' : '${agg.items.length}',
                  ),
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: _StatCard(
                    icon: Icons.star_border,
                    label: '收藏',
                    value: agg == null ? '-' : '$collected',
                  ),
                ),
              ],
            ),
            const SizedBox(height: 8),
            const Text(
              '口径：跨设备 LWS 归并（最近学习设备整行胜出）；'
              '墨封 = 设备端标记已掌握同步上报。',
              style: TextStyle(fontSize: 11, color: Colors.black45),
            ),
          ],
        ),
      ),
    );
  }

  Widget _readingCard(BuildContext context, ColorScheme scheme) {
    if (state._selectedDeviceId == null) return const SizedBox.shrink();
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(14),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Icon(Icons.auto_stories, color: scheme.primary, size: 20),
                const SizedBox(width: 8),
                const Expanded(
                  child: Text(
                    '阅读记录',
                    style: TextStyle(fontSize: 15, fontWeight: FontWeight.w600),
                  ),
                ),
              ],
            ),
            if (state._readings.isEmpty)
              const Padding(
                padding: EdgeInsets.symmetric(vertical: 10),
                child: Text(
                  '暂无阅读记录（设备端「我的书架」下载书籍后开始阅读）',
                  style: TextStyle(color: Colors.black54, fontSize: 13),
                ),
              )
            else ...[
              for (final r in state._readings.take(5)) _readingRow(r),
            ],
          ],
        ),
      ),
    );
  }

  Widget _readingRow(DeviceReading r) {
    final last = r.lastReadAt == null
        ? ''
        : '最近阅读 ${r.lastReadAt!.month}/${r.lastReadAt!.day}';
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 6),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Expanded(
                child: Text(
                  r.title,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(fontWeight: FontWeight.w500),
                ),
              ),
              Text(
                '$totalMinutesLabel(r) · $last',
                style: const TextStyle(fontSize: 12, color: Colors.black45),
              ),
            ],
          ),
          const SizedBox(height: 4),
          Row(
            children: [
              Expanded(
                child: ClipRRect(
                  borderRadius: BorderRadius.circular(4),
                  child: LinearProgressIndicator(
                    value: r.progressPct / 100,
                    minHeight: 6,
                  ),
                ),
              ),
              const SizedBox(width: 8),
              Text('${r.progressPct}%', style: const TextStyle(fontSize: 12)),
            ],
          ),
        ],
      ),
    );
  }

  String totalMinutesLabel(DeviceReading r) =>
      r.totalReadMinutes <= 0 ? '未计时' : '累计 ${r.totalReadMinutes} 分钟';

  Widget _libraryCard(BuildContext context) {
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(14),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Icon(
                  Icons.cloud_queue,
                  color: Theme.of(context).colorScheme.primary,
                  size: 20,
                ),
                const SizedBox(width: 8),
                const Expanded(
                  child: Text(
                    '云端书库',
                    style: TextStyle(fontSize: 15, fontWeight: FontWeight.w600),
                  ),
                ),
                Text(
                  '${state._books.length} 本',
                  style: const TextStyle(fontSize: 12, color: Colors.black45),
                ),
              ],
            ),
            if (state._books.isEmpty)
              const Padding(
                padding: EdgeInsets.symmetric(vertical: 10),
                child: Text(
                  '云端暂无已发布书籍',
                  style: TextStyle(color: Colors.black54, fontSize: 13),
                ),
              )
            else ...[
              for (final b
                  in state._booksExpanded ? state._books : state._books.take(8))
                ListTile(
                  contentPadding: EdgeInsets.zero,
                  dense: true,
                  leading: const Icon(Icons.book_outlined, size: 20),
                  title: Text(
                    b.title,
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                  ),
                  subtitle: Text(
                    '${b.author.isEmpty ? "佚名" : b.author} · ${_sizeLabel(b.fileSize)} · 下载 ${b.downloadCount} 次',
                    style: const TextStyle(fontSize: 12),
                  ),
                ),
              if (state._books.length > 8)
                Align(
                  alignment: Alignment.centerLeft,
                  child: TextButton(
                    onPressed: onToggleBooks,
                    child: Text(
                      state._booksExpanded
                          ? '收起'
                          : '展开全部（${state._books.length} 本）',
                      style: const TextStyle(fontSize: 12),
                    ),
                  ),
                ),
            ],
            const SizedBox(height: 4),
            const Text(
              '只读浏览：下载请在设备端「我的书架 → 云端书架」操作。',
              style: TextStyle(fontSize: 11, color: Colors.black45),
            ),
          ],
        ),
      ),
    );
  }

  String _sizeLabel(int bytes) {
    if (bytes > 1024 * 1024) {
      return '${(bytes / 1024 / 1024).toStringAsFixed(1)} MB';
    }
    return '${bytes ~/ 1024} KB';
  }
}

// ==================== 共享小组件 ====================

class _SectionTitle extends StatelessWidget {
  final IconData icon;
  final String text;
  const _SectionTitle({required this.icon, required this.text});

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.only(left: 4, bottom: 8),
      child: Row(
        children: [
          Icon(icon, size: 18, color: Theme.of(context).colorScheme.primary),
          const SizedBox(width: 6),
          Text(
            text,
            style: const TextStyle(fontSize: 14, fontWeight: FontWeight.w600),
          ),
        ],
      ),
    );
  }
}

class _StatCard extends StatelessWidget {
  final IconData icon;
  final String label;
  final String value;
  const _StatCard({
    required this.icon,
    required this.label,
    required this.value,
  });

  @override
  Widget build(BuildContext context) {
    return Card(
      child: Padding(
        padding: const EdgeInsets.symmetric(vertical: 14),
        child: Column(
          children: [
            Icon(icon, size: 26, color: Theme.of(context).colorScheme.primary),
            const SizedBox(height: 6),
            Text(
              value,
              style: const TextStyle(fontSize: 22, fontWeight: FontWeight.bold),
            ),
            Text(
              label,
              style: const TextStyle(fontSize: 12, color: Colors.black54),
            ),
          ],
        ),
      ),
    );
  }
}
