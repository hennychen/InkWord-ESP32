import { Injectable, inject } from '@angular/core';
import { HttpClient, HttpParams } from '@angular/common/http';
import { Observable } from 'rxjs';
import { environment } from '../../../environments/environment';
import {
  BookListItem, BookDetail, BookCreateDto, BookUpdateDto, BookQuery,
  PagedResult, ApiResponse,
} from '../models/models';

/**
 * 书籍管理 API 服务（阅读器后端，2026-09）。
 * 对接 AdminBookController：上传 / 元数据 / 发布 / 删除 / 详情。
 * 上传默认草稿态，需手动发布后设备端云端书架才可见。
 */
@Injectable({ providedIn: 'root' })
export class BookApiService {
  private readonly http = inject(HttpClient);
  private readonly base = `${environment.apiUrl}/admin/books`;

  /** 书籍列表（分页 + 关键词/语言/发布态过滤） */
  query(params: BookQuery): Observable<ApiResponse<PagedResult<BookListItem>>> {
    let httpParams = new HttpParams()
      .set('page', params.page)
      .set('size', params.size);
    if (params.keyword) httpParams = httpParams.set('keyword', params.keyword);
    if (params.language) httpParams = httpParams.set('language', params.language);
    if (params.published != null) httpParams = httpParams.set('published', params.published);

    return this.http.get<ApiResponse<PagedResult<BookListItem>>>(this.base, { params: httpParams });
  }

  /** 上传书籍（multipart：file + 元数据；TXT/MD/HTML ≤10MB） */
  upload(file: File, meta: BookCreateDto): Observable<ApiResponse<{ id: string; bookKey: string; title: string }>> {
    const formData = new FormData();
    formData.append('file', file);
    formData.append('Title', meta.title);
    formData.append('Author', meta.author ?? '');
    formData.append('Language', meta.language);
    formData.append('Tags', meta.tags ?? '');
    formData.append('Description', meta.description ?? '');
    return this.http.post<ApiResponse<{ id: string; bookKey: string; title: string }>>(
      `${this.base}/upload`, formData);
  }

  /** 更新元数据（含发布态） */
  update(id: string, dto: BookUpdateDto): Observable<ApiResponse<null>> {
    return this.http.put<ApiResponse<null>>(`${this.base}/${id}`, dto);
  }

  /** 删除（软删除记录 + 移除书籍文件） */
  delete(id: string): Observable<ApiResponse<null>> {
    return this.http.delete<ApiResponse<null>>(`${this.base}/${id}`);
  }

  /** 发布 / 取消发布（发布后设备端云端书架可见） */
  publish(id: string, publish: boolean): Observable<ApiResponse<null>> {
    return this.http.post<ApiResponse<null>>(`${this.base}/${id}/publish`, null, {
      params: { publish },
    });
  }

  /** 书籍详情（含 readerCount / avgProgressPct 阅读统计） */
  detail(id: string): Observable<ApiResponse<BookDetail>> {
    return this.http.get<ApiResponse<BookDetail>>(`${this.base}/${id}`);
  }
}
