import { Injectable, inject } from '@angular/core';
import { HttpClient, HttpParams } from '@angular/common/http';
import { Observable } from 'rxjs';
import { environment } from '../../../environments/environment';
import {
  Word, WordCreateDto, WordUpdateDto, WordQueryDto,
  PagedResult, ApiResponse,
} from '../models/models';

/**
 * 词库管理 API 服务 (A-04 / B-13~B-15)。
 * 封装 CRUD、分页搜索、CSV 批量导入。
 */
@Injectable({ providedIn: 'root' })
export class WordApiService {
  private readonly http = inject(HttpClient);
  private readonly base = `${environment.apiUrl}/admin/words`;

  /** 分页查询 */
  query(params: WordQueryDto): Observable<ApiResponse<PagedResult<Word>>> {
    let httpParams = new HttpParams()
      .set('page', params.page)
      .set('size', params.size);
    if (params.keyword) httpParams = httpParams.set('keyword', params.keyword);
    if (params.tag) httpParams = httpParams.set('tag', params.tag);
    if (params.difficulty != null) httpParams = httpParams.set('difficulty', params.difficulty);

    return this.http.get<ApiResponse<PagedResult<Word>>>(this.base, { params: httpParams });
  }

  /** 获取单个词条 */
  getById(id: string): Observable<ApiResponse<Word>> {
    return this.http.get<ApiResponse<Word>>(`${this.base}/${id}`);
  }

  /** 新增 */
  create(dto: WordCreateDto): Observable<ApiResponse<Word>> {
    return this.http.post<ApiResponse<Word>>(this.base, dto);
  }

  /** 修改 */
  update(id: string, dto: WordUpdateDto): Observable<ApiResponse<Word>> {
    return this.http.put<ApiResponse<Word>>(`${this.base}/${id}`, dto);
  }

  /** 软删除 */
  delete(id: string): Observable<ApiResponse<null>> {
    return this.http.delete<ApiResponse<null>>(`${this.base}/${id}`);
  }

  /** 批量导入 CSV */
  importCsv(file: File): Observable<ApiResponse<{ success: number; failed: number }>> {
    const formData = new FormData();
    formData.append('file', file);
    return this.http.post<ApiResponse<{ success: number; failed: number }>>(
      `${this.base}/import`, formData,
    );
  }

  /** 导出设备词库文件（P2：含 cloudId，拷入 SD 卡后设备可上报评分/收藏） */
  exportDeviceLibrary(): Observable<Blob> {
    return this.http.get(`${this.base}/export`, { responseType: 'blob' });
  }
}
