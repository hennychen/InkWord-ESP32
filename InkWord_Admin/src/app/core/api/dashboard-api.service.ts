import { Injectable, inject } from '@angular/core';
import { HttpClient } from '@angular/common/http';
import { Observable } from 'rxjs';
import { environment } from '../../../environments/environment';
import {
  DashboardStats, SrsDistribution, DailyActiveData, ApiResponse,
} from '../models/models';

/**
 * 数据看板 API 服务 (A-04 / B-18)。
 * 获取今日活跃设备、学习用户、SRS 分布、日活趋势等统计数据。
 */
@Injectable({ providedIn: 'root' })
export class DashboardApiService {
  private readonly http = inject(HttpClient);
  private readonly base = `${environment.apiUrl}/admin/dashboard`;

  /** 看板总览统计 */
  getStats(): Observable<ApiResponse<DashboardStats>> {
    return this.http.get<ApiResponse<DashboardStats>>(`${this.base}/stats`);
  }

  /** SRS 分布饼图数据 */
  getSrsDistribution(): Observable<ApiResponse<SrsDistribution[]>> {
    return this.http.get<ApiResponse<SrsDistribution[]>>(`${this.base}/srs-distribution`);
  }

  /** 日活趋势（最近 N 天） */
  getDailyActive(days = 30): Observable<ApiResponse<DailyActiveData[]>> {
    return this.http.get<ApiResponse<DailyActiveData[]>>(
      `${this.base}/daily-active`, { params: { days } },
    );
  }
}
