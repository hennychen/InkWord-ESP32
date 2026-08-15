import { Injectable, inject } from '@angular/core';
import { HttpClient } from '@angular/common/http';
import { Observable } from 'rxjs';
import { environment } from '../../../environments/environment';
import { OtaPackage, ApiResponse } from '../models/models';

/**
 * OTA 升级包管理 API 服务 (A-04 / B-17)。
 * 上传固件、查询版本列表。
 */
@Injectable({ providedIn: 'root' })
export class OtaApiService {
  private readonly http = inject(HttpClient);
  private readonly base = `${environment.apiUrl}/admin/ota`;

  /** OTA 版本列表 */
  list(): Observable<ApiResponse<OtaPackage[]>> {
    return this.http.get<ApiResponse<OtaPackage[]>>(this.base);
  }

  /** 上传固件包 */
  upload(file: File, version: string, description?: string): Observable<ApiResponse<OtaPackage>> {
    const formData = new FormData();
    formData.append('file', file);
    formData.append('version', version);
    if (description) formData.append('description', description);
    return this.http.post<ApiResponse<OtaPackage>>(`${this.base}/upload`, formData);
  }
}
