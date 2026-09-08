import { Injectable, inject } from '@angular/core';
import { HttpClient, HttpParams } from '@angular/common/http';
import { Observable } from 'rxjs';
import { environment } from '../../../environments/environment';
import {
  ApiResponse, AdminSubjectItem, DeckListItem, DeckDetail,
  DeckItemsResp, DeckItemReq,
} from '../models/models';

/**
 * 卡组管理 API 服务（P3，2026-09）：
 * 科目清单 + 卡组列表（科目过滤）+ 详情（学习覆盖统计）
 * + 条目分页/CRUD（FillWord/NextVersion 后端同源契约）
 * + 卡组字符集导出（T4.5 生僻字子集核对）。
 */
@Injectable({ providedIn: 'root' })
export class DeckApiService {
  private readonly http = inject(HttpClient);
  private readonly base = `${environment.apiUrl}/admin/decks`;
  private readonly subjectBase = `${environment.apiUrl}/admin/subjects`;

  /** 科目列表（chip 侧栏数据源，含每科目卡组计数） */
  subjects(): Observable<ApiResponse<AdminSubjectItem[]>> {
    return this.http.get<ApiResponse<AdminSubjectItem[]>>(this.subjectBase);
  }

  /** 卡组列表（subject= 科目 Code 过滤；'' = 全量） */
  decks(subject?: string): Observable<ApiResponse<DeckListItem[]>> {
    let params = new HttpParams();
    if (subject) params = params.set('subject', subject);
    return this.http.get<ApiResponse<DeckListItem[]>>(this.base, { params });
  }

  /** 卡组详情（学习覆盖统计：learnerCount / studiedItems） */
  detail(id: string): Observable<ApiResponse<DeckDetail>> {
    return this.http.get<ApiResponse<DeckDetail>>(`${this.base}/${id}`);
  }

  /** 条目分页（Version 升序 = 录入序；含归档行删除痕迹） */
  items(id: string, page: number, size: number): Observable<ApiResponse<DeckItemsResp>> {
    const params = new HttpParams().set('page', page).set('size', size);
    return this.http.get<ApiResponse<DeckItemsResp>>(`${this.base}/${id}/items`, { params });
  }

  /** 新增条目（uq(Text,Tag) 冲突 409） */
  addItem(id: string, req: DeckItemReq): Observable<ApiResponse<{ id: string; version: number }>> {
    return this.http.post<ApiResponse<{ id: string; version: number }>>(
      `${this.base}/${id}/items`, req);
  }

  /** 改条目（Version 接全局 max 递增，设备增量同步下发契约） */
  updateItem(
    id: string, wordId: string, req: DeckItemReq,
  ): Observable<ApiResponse<{ id: string; version: number }>> {
    return this.http.put<ApiResponse<{ id: string; version: number }>>(
      `${this.base}/${id}/items/${wordId}`, req);
  }

  /** 删条目（归档 + Version 接 max 递增；GetIncrementalAsync 排除归档行
   * ——已同步设备不感知，需 App LAN 重推或重新导出词库覆盖，同 me 端删除语义） */
  deleteItem(id: string, wordId: string): Observable<ApiResponse<null>> {
    return this.http.delete<ApiResponse<null>>(`${this.base}/${id}/items/${wordId}`);
  }

  /** 卡组字符集（T4.5）：全部条目可渲染九列去重字符流，
   * text/plain 非信封；喂 tools/gen_cjk_font.swift --subset 生成子集字库 */
  charsetText(code: string): Observable<string> {
    return this.http.get(`${this.base}/${encodeURIComponent(code)}/charset`, {
      responseType: 'text',
    });
  }
}
