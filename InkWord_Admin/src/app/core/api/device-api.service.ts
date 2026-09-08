import { Injectable, inject } from '@angular/core';
import { HttpClient } from '@angular/common/http';
import { Observable } from 'rxjs';
import { environment } from '../../../environments/environment';
import { Device, DeviceCommandDto, DeviceBookReadingItem, ApiResponse } from '../models/models';

/**
 * 设备管理 API 服务 (A-04 / B-16)。
 * 查看设备列表、详情，下发远程指令。
 */
@Injectable({ providedIn: 'root' })
export class DeviceApiService {
  private readonly http = inject(HttpClient);
  private readonly base = `${environment.apiUrl}/admin/devices`;

  /** 设备列表 */
  list(): Observable<ApiResponse<Device[]>> {
    return this.http.get<ApiResponse<Device[]>>(this.base);
  }

  /** 设备详情 */
  getById(id: string): Observable<ApiResponse<Device>> {
    return this.http.get<ApiResponse<Device>>(`${this.base}/${id}`);
  }

  /** 下发远程指令（强制同步 / 强制全刷 / 推送 OTA） */
  sendCommand(dto: DeviceCommandDto): Observable<ApiResponse<null>> {
    return this.http.post<ApiResponse<null>>(`${this.base}/${dto.deviceId}/command`, dto);
  }

  /** 设备阅读记录（阅读器后端：ReadingProgress 按最近阅读倒序） */
  deviceReading(deviceId: string): Observable<ApiResponse<{ books: DeviceBookReadingItem[] }>> {
    return this.http.get<ApiResponse<{ books: DeviceBookReadingItem[] }>>(
      `${environment.apiUrl}/admin/dashboard/device-reading/${deviceId}`);
  }
}
