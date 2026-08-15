import { Component, OnInit, inject, signal, computed } from '@angular/core';
import { CommonModule } from '@angular/common';
import { MatCardModule } from '@angular/material/card';
import { MatProgressSpinnerModule } from '@angular/material/progress-spinner';
import { MatIconModule } from '@angular/material/icon';
import { NgxEchartsDirective } from 'ngx-echarts';
import type { EChartsOption } from 'echarts';
import { DashboardApiService } from '../../core/api/dashboard-api.service';
import {
  DashboardStats, SrsDistribution, DailyActiveData,
} from '../../core/models/models';

/**
 * 数据看板大屏 (A-05)：
 * - 卡片：总词数 / 总设备数 / 今日活跃设备 / 今日学习用户 / 平均学习时长
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

  ngOnInit(): void {
    this.loadAll();
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
  }
}
