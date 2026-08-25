import { Injectable, inject } from '@angular/core';
import { HttpClient, HttpParams } from '@angular/common/http';
import { Observable } from 'rxjs';
import { environment } from '../../../environments/environment';
import {
  Word, WordCreateDto, WordUpdateDto, WordQueryDto,
  PagedResult, ApiResponse, AiGenerateReq, AiPendingItem, AiApplyReq,
  DeckGenReq, AdminDeckInfo,
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

  // ====== AI 内容增强（M1 路径 B，2026-08-22）======

  /** 手动触发 AI 批量生成（立即入 Hangfire 队列，进度见 /hangfire） */
  aiGenerate(req: AiGenerateReq): Observable<ApiResponse<{ jobId: string }>> {
    return this.http.post<ApiResponse<{ jobId: string }>>(`${this.base}/ai-generate`, req);
  }

  /** 待审建议分页（AiStatus=1，现值 vs 建议 diff 视图） */
  aiPending(page: number, size: number): Observable<ApiResponse<PagedResult<AiPendingItem>>> {
    const params = new HttpParams().set('page', page).set('size', size);
    return this.http.get<ApiResponse<PagedResult<AiPendingItem>>>(
      `${this.base}/ai-pending`, { params });
  }

  /** 待审数量（词库页角标） */
  aiPendingCount(): Observable<ApiResponse<{ count: number }>> {
    return this.http.get<ApiResponse<{ count: number }>>(`${this.base}/ai-pending/count`);
  }

  /** 审核通过（可携带编辑终值；null = 采用建议原值） */
  aiApply(id: string, req: AiApplyReq): Observable<ApiResponse<Word>> {
    return this.http.post<ApiResponse<Word>>(`${this.base}/ai-apply/${id}`, req);
  }

  /** 驳回：状态复位 0（可重新生成） */
  aiReject(id: string): Observable<ApiResponse<null>> {
    return this.http.post<ApiResponse<null>>(`${this.base}/ai-reject/${id}`, null);
  }

  // ====== AI 卡组生成（v1.5 T5.4）======

  private readonly deckBase = `${environment.apiUrl}/admin/decks`;

  /** 管理端卡组列表（AI 生成弹窗下拉；含条目计数） */
  adminDecks(): Observable<ApiResponse<AdminDeckInfo[]>> {
    return this.http.get<ApiResponse<AdminDeckInfo[]>>(this.deckBase);
  }

  /** AI 批量生成卡组：素材→占位待审条目（审核台通过后 Version++ 下发） */
  deckAiGenerate(id: string, req: DeckGenReq): Observable<ApiResponse<{ jobId: string }>> {
    return this.http.post<ApiResponse<{ jobId: string }>>(
      `${this.deckBase}/${id}/ai-generate`, req);
  }
}
