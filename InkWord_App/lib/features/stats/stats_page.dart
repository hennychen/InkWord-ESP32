/// 学习统计页（v1.3 T3.4）：今日统计 / 连续天数可视化
///
/// 数据走设备 GET /api/stats（lr_stats 口径：今日首评 / 今日评分总次数 /
/// 连续天数 + 错词 / 到期 / 收藏数），局域网直读无需云端。
library;

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../../services/http_device_client.dart';
import '../../state/device_controller.dart';

class StatsPage extends StatefulWidget {
  const StatsPage({super.key});

  @override
  State<StatsPage> createState() => _StatsPageState();
}

class _StatsPageState extends State<StatsPage> {
  DeviceStats? _stats;
  String? _error;
  bool _loading = true;

  DeviceHttpClient? get _client => context.read<DeviceController>().client;

  @override
  void initState() {
    super.initState();
    _reload();
  }

  Future<void> _reload() async {
    final c = _client;
    if (c == null) {
      setState(() => _loading = false);
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

  @override
  Widget build(BuildContext context) {
    final connected = context.watch<DeviceController>().connected;
    return Scaffold(
      appBar: AppBar(title: const Text('学习统计')),
      body: !connected
          ? const Center(
              child: Padding(
                padding: EdgeInsets.all(24),
                child: Text(
                  '未连接设备：请先在「设备」页连接\n（BLE / mDNS / IP）',
                  textAlign: TextAlign.center,
                  style: TextStyle(color: Colors.black54),
                ),
              ),
            )
          : RefreshIndicator(
              onRefresh: _reload,
              child: _loading
                  ? const Center(child: CircularProgressIndicator())
                  : _error != null
                  ? ListView(
                      children: [
                        Card(
                          margin: const EdgeInsets.all(12),
                          child: Padding(
                            padding: const EdgeInsets.all(14),
                            child: Text('读取失败：$_error'),
                          ),
                        ),
                      ],
                    )
                  : _StatsView(stats: _stats!),
            ),
    );
  }
}

class _StatsView extends StatelessWidget {
  final DeviceStats stats;
  const _StatsView({required this.stats});

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    return ListView(
      padding: const EdgeInsets.all(12),
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
                    Text('当前词书'),
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
