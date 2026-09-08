import { Component, OnInit, inject, signal, computed } from '@angular/core';
import { CommonModule } from '@angular/common';
import { MatCardModule } from '@angular/material/card';
import { MatProgressSpinnerModule } from '@angular/material/progress-spinner';
import { MatIconModule } from '@angular/material/icon';
import { MatTableModule } from '@angular/material/table';
import { NgxEchartsDirective } from 'ngx-echarts';
import type { EChartsOption } from 'echarts';
import { DashboardApiService } from '../../core/api/dashboard-api.service';
import {
  DashboardStats, SrsDistribution, DailyActiveData, SrsComparisonResp, TodayStats,
  ReadingStatsResp,
} from '../../core/models/models';

/**
 * 数据看板大屏 (A-05)：
 * - 卡片：总词数 / 总设备数 / 今日活跃设备 / 今日学习用户 / 平均学习时长
 * - 今日学习分析条（v1.3 T3.2）：首学/复习/正确率/平均质量
 * - 折线图：日活趋势
 * - 饼图：SRS 分布
 * 异步加载数据时显示 Loading 骨架屏。
 */
@Component({
  selector: 'app-dashboard',
  standalone: true,
  imports: [
    CommonModule,
    MatCardModule,
    MatProgressSpinnerModule,
    MatIconModule,
    MatTableModule,
    NgxEchartsDirective,
  ],
  templateUrl: './dashboard.component.html',
  styleUrl: './dashboard.component.scss',
})
export class DashboardComponent implements OnInit {
  private readonly dashboardApi = inject(DashboardApiService);

  readonly loading = signal(true);
  readonly stats = signal<DashboardStats | null>(null);
  readonly srsData = signal<SrsDistribution[]>([]);
  readonly dailyData = signal<DailyActiveData[]>([]);
  /** SM-2 vs FSRS 影子对比（M3 路径 A：影子运行切换决策依据） */
  readonly srsComparison = signal<SrsComparisonResp | null>(null);
  /** 今日学习统计（v1.3 T3.2：LearningRecord 按日聚合） */
  readonly todayStats = signal<TodayStats | null>(null);
  /** 阅读统计（阅读器后端 2026-09：今日读者/时长/热门书/近 7 天趋势） */
  readonly readingStats = signal<ReadingStatsResp | null>(null);
  /** 热门书籍表列 */
  readonly popularColumns = ['rank', 'title', 'readers', 'progress'];

  /** 今日正确率（%）：答对 /（答对 + 答错），无人次时为 0 */
  readonly correctRate = computed(() => {
    const t = this.todayStats();
    if (!t) return 0;
    const total = t.correctToday + t.wrongToday;
    return total === 0 ? 0 : Math.round((t.correctToday / total) * 100);
  });

  /** 折线图配置 */
  readonly lineChartOption = computed<EChartsOption>(() => {
    const data = this.dailyData();
    return {
      tooltip: { trigger: 'axis' },
      xAxis: {
        type: 'category',
        data: data.map(d => d.date),
        axisLabel: { rotate: 30 },
      },
      yAxis: { type: 'value', name: '活跃数' },
      series: [{
        name: '日活',
        type: 'line',
        smooth: true,
        data: data.map(d => d.count),
        areaStyle: { opacity: 0.15 },
        itemStyle: { color: '#1a1a1a' },
      }],
      grid: { left: '5%', right: '5%', bottom: '10%', containLabel: true },
    };
  });

  /** 饼图配置 */
  readonly pieChartOption = computed<EChartsOption>(() => {
    const data = this.srsData();
    return {
      tooltip: { trigger: 'item' },
      legend: { bottom: 0 },
      series: [{
        type: 'pie',
        radius: ['40%', '70%'],
        data: data.map(d => ({ name: d.level, value: d.count })),
        emphasis: { itemStyle: { shadowBlur: 10, shadowOffsetX: 0, shadowColor: 'rgba(0,0,0,0.5)' } },
      }],
    };
  });

  /** 阅读趋势折线图（近 7 天阅读分钟数 + 读者数，复用 lineChartOption 范式） */
  readonly readingChartOption = computed<EChartsOption>(() => {
    const data = this.readingStats()?.dailyReading ?? [];
    return {
      tooltip: { trigger: 'axis' },
      legend: { bottom: 0 },
      xAxis: {
        type: 'category',
        data: data.map(d => d.date.slice(5)),
        axisLabel: { rotate: 30 },
      },
      yAxis: [
        { type: 'value', name: '分钟' },
        { type: 'value', name: '读者' },
      ],
      series: [
        {
          name: '阅读分钟',
          type: 'line',
          smooth: true,
          data: data.map(d => d.minutes),
          areaStyle: { opacity: 0.15 },
          itemStyle: { color: '#1a1a1a' },
        },
        {
          name: '活跃读者',
          type: 'line',
          smooth: true,
          yAxisIndex: 1,
          data: data.map(d => d.readers),
          itemStyle: { color: '#78909c' },
        },
      ],
      grid: { left: '5%', right: '5%', bottom: '12%', containLabel: true },
    };
  });

  /** 分组柱状图：SM-2 vs FSRS 到期分布对比（影子运行观察窗口） */
  readonly comparisonChartOption = computed<EChartsOption>(() => {
    const items = this.srsComparison()?.items ?? [];
    // 后端 algorithm 取值 "sm2"/"fsrs"（小写）
    const names = items.map((i) => (i.algorithm === 'sm2' ? 'SM-2（现行）' : 'FSRS（影子）'));
    return {
      tooltip: { trigger: 'axis' },
      legend: { bottom: 0 },
      xAxis: { type: 'category', data: names },
      yAxis: { type: 'value', name: '到期词数' },
      series: [
        { name: '今日', type: 'bar', data: items.map((i) => i.dueToday) },
        { name: '本周', type: 'bar', data: items.map((i) => i.dueWeek) },
        { name: '本月', type: 'bar', data: items.map((i) => i.dueMonth) },
        { name: '更远', type: 'bar', data: items.map((i) => i.future) },
      ],
      grid: { left: '5%', right: '5%', bottom: '12%', containLabel: true },
    };
  });

  ngOnInit(): void {
    this.loadAll();
  }

  /** 对比图副标题：算法条目平均间隔（天，缺位安全） */
  avgInterval(index: number): string {
    const item = this.srsComparison()?.items[index];
    return item ? item.avgIntervalDays.toFixed(1) : '—';
  }

  private loadAll(): void {
    this.loading.set(true);

    // 加载统计概览
    this.dashboardApi.getStats().subscribe({
      next: (res) => this.stats.set(res.data ?? null),
      complete: () => this.loading.set(false),
    });

    // 加载 SRS 分布
    this.dashboardApi.getSrsDistribution().subscribe({
      next: (res) => this.srsData.set(res.data ?? []),
    });

    // 加载日活趋势
    this.dashboardApi.getDailyActive(30).subscribe({
      next: (res) => this.dailyData.set(res.data ?? []),
    });

    // 加载 SM-2 vs FSRS 对比（影子数据，无记录时图表留空）
    this.dashboardApi.getSrsComparison().subscribe({
      next: (res) => this.srsComparison.set(res.data ?? null),
    });

    // 加载今日学习统计（v1.3 T3.2）
    this.dashboardApi.getTodayStats().subscribe({
      next: (res) => this.todayStats.set(res.data ?? null),
    });

    // 加载阅读统计（阅读器后端 2026-09；无阅读数据时区域留空）
    this.dashboardApi.getReadingStats().subscribe({
      next: (res) => this.readingStats.set(res.data ?? null),
    });
  }
}
