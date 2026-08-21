import { Component, OnInit, inject, signal, computed } from '@angular/core';
import { CommonModule } from '@angular/common';
import { MatCardModule } from '@angular/material/card';
import { MatTableModule } from '@angular/material/table';
import { MatProgressSpinnerModule } from '@angular/material/progress-spinner';
import { NgxEchartsDirective } from 'ngx-echarts';
import type { EChartsOption } from 'echarts';
import { DashboardApiService } from '../../core/api/dashboard-api.service';
import { WrongTopResp } from '../../core/models/models';

/**
 * 错词排行榜 (P1)：
 * - 横向条形图：连错总次数 Top 10（跨学习者聚合）
 * - 表格：单词 / 释义 / 连错次数 / 涉及学习者数
 * 数据源 GET /admin/dashboard/wrong-top（ConsecutiveWrong>0 聚合，
 * 连错口径与固件 learning_state / 后端 SrsService 同步：
 * quality<3 递增、>=3 清零）。
 */
@Component({
  selector: 'app-wrong-top',
  standalone: true,
  imports: [
    CommonModule,
    MatCardModule,
    MatTableModule,
    MatProgressSpinnerModule,
    NgxEchartsDirective,
  ],
  templateUrl: './wrong-top.component.html',
  styleUrl: './wrong-top.component.scss',
})
export class WrongTopComponent implements OnInit {
  private readonly dashboardApi = inject(DashboardApiService);

  readonly loading = signal(true);
  readonly resp = signal<WrongTopResp | null>(null);

  readonly displayedColumns: string[] = [
    'rank', 'wordText', 'meaning', 'wrongCount', 'learners',
  ];

  /** 横向条形图（Top 10，reverse 使榜首在顶部） */
  readonly chartOption = computed<EChartsOption>(() => {
    const items = (this.resp()?.items ?? []).slice(0, 10).reverse();
    return {
      tooltip: { trigger: 'axis', axisPointer: { type: 'shadow' } },
      grid: { left: '5%', right: '10%', top: '5%', bottom: '5%', containLabel: true },
      xAxis: { type: 'value', name: '连错次数' },
      yAxis: { type: 'category', data: items.map(i => i.wordText) },
      series: [{
        name: '连错次数',
        type: 'bar',
        data: items.map(i => i.wrongCount),
        itemStyle: { color: '#b71c1c' },
        barMaxWidth: 22,
        label: { show: true, position: 'right' },
      }],
    };
  });

  ngOnInit(): void {
    this.dashboardApi.getWrongTop(20).subscribe({
      next: (res) => this.resp.set(res.data ?? null),
      error: () => this.resp.set(null),
      complete: () => this.loading.set(false),
    });
  }
}
