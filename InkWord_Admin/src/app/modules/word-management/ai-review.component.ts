import { Component, OnInit, inject, signal } from '@angular/core';
import { CommonModule } from '@angular/common';
import { MatButtonModule } from '@angular/material/button';
import { MatIconModule } from '@angular/material/icon';
import { MatCardModule } from '@angular/material/card';
import { MatPaginatorModule, PageEvent } from '@angular/material/paginator';
import { MatProgressSpinnerModule } from '@angular/material/progress-spinner';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatInputModule } from '@angular/material/input';
import { MatChipsModule } from '@angular/material/chips';
import { MatSnackBar } from '@angular/material/snack-bar';
import { Router } from '@angular/router';
import { FormsModule } from '@angular/forms';
import { WordApiService } from '../../core/api/word-api.service';
import { AiPendingItem } from '../../core/models/models';

/**
 * AI 审核页 (M1 路径 B，2026-08-22)：
 * 分页拉取 AiStatus=1 待审建议，卡片流展示「原值 vs AI 建议」diff，
 * 人工比对后 通过（可编辑终值）/ 驳回（复位可重试）。
 * 红线：审核通过前建议绝不落词库字段（防 LLM 幻觉污染）。
 */
@Component({
  selector: 'app-ai-review',
  standalone: true,
  imports: [
    CommonModule,
    FormsModule,
    MatButtonModule,
    MatIconModule,
    MatCardModule,
    MatPaginatorModule,
    MatProgressSpinnerModule,
    MatFormFieldModule,
    MatInputModule,
    MatChipsModule,
  ],
  templateUrl: './ai-review.component.html',
  styleUrl: './ai-review.component.scss',
})
export class AiReviewComponent implements OnInit {
  private readonly wordApi = inject(WordApiService);
  private readonly snackBar = inject(MatSnackBar);
  private readonly router = inject(Router);

  readonly loading = signal(true);
  readonly items = signal<AiPendingItem[]>([]);
  readonly total = signal(0);
  /** 本页卡片的编辑态（id → 编辑后终值），通过时作为 req 载荷 */
  readonly edits = signal<Record<string, string>>({});
  /** 操作进行中（防重复点击） */
  readonly busy = signal<string | null>(null);

  pageSize = 10;
  currentPage = 0;

  readonly kindLabels: Record<number, string> = {
    0: '分级例句 → Example',
    1: '词根助记 → Root',
    2: '易混辨析（仅参考）',
    3: '卡组条目生成（T5.4）',
  };

  ngOnInit(): void {
    this.loadPage();
  }

  onPageChange(e: PageEvent): void {
    this.currentPage = e.pageIndex;
    this.pageSize = e.pageSize;
    this.loadPage();
  }

  private loadPage(): void {
    this.loading.set(true);
    this.wordApi.aiPending(this.currentPage + 1, this.pageSize).subscribe({
      next: (res) => {
        const result = res.data;
        this.items.set(result?.items ?? []);
        this.total.set(result?.total ?? 0);
        // 编辑态初始化：kind 0/1/3 预填建议原值（可改），kind 2 无可编辑字段；
        // kind 3（T5.4 卡组条目）编辑背面（题面/拼音/译文取建议原值）
        const next: Record<string, string> = {};
        for (const it of this.items()) {
          const text = it.kind === 0 ? it.suggestedExample
            : it.kind === 3 ? it.suggestedBack
            : it.suggestedRoot;
          if ((it.kind === 0 || it.kind === 1 || it.kind === 3) && text != null) next[it.id] = text;
        }
        this.edits.set(next);
        this.loading.set(false);
      },
      error: () => this.loading.set(false),
    });
  }

  /** 卡片当前生效的编辑文本 */
  editedText(item: AiPendingItem): string {
    return this.edits()[item.id] ?? '';
  }

  onEdit(item: AiPendingItem, value: string): void {
    this.edits.update((m) => ({ ...m, [item.id]: value }));
  }

  /** 当前值（kind 对应字段） */
  currentValue(item: AiPendingItem): string {
    return item.kind === 1 ? item.currentRoot : item.currentExample;
  }

  /** 建议展示文本（kind 0/1/2 各取对应字段） */
  suggestedText(item: AiPendingItem): string {
    if (item.kind === 1) return item.suggestedRoot ?? '';
    if (item.kind === 2) return item.suggestedConfusionNote ?? '';
    return item.suggestedExample ?? '';
  }

  apply(item: AiPendingItem): void {
    this.busy.set(item.id);
    const req = item.kind === 2
      ? {} // 易混辨析仅审阅参考：通过即标记已审，不落设备字段
      : item.kind === 0
        ? { example: this.editedText(item).trim() || null }
        : item.kind === 3
          ? { back: this.editedText(item).trim() || null } // T5.4：编辑终值仅背面
          : { root: this.editedText(item).trim() || null };
    this.wordApi.aiApply(item.id, req).subscribe({
      next: () => {
        this.snackBar.open(`"${item.text}" 已通过，版本已递增待设备增量同步`, '关闭', {
          duration: 3000,
        });
        this.removeLocal(item.id);
        this.busy.set(null);
      },
      error: () => this.busy.set(null),
    });
  }

  reject(item: AiPendingItem): void {
    if (!confirm(`驳回 "${item.text}" 的 AI 建议？状态将复位，可重新生成。`)) return;
    this.busy.set(item.id);
    this.wordApi.aiReject(item.id).subscribe({
      next: () => {
        this.snackBar.open(`"${item.text}" 已驳回`, '关闭', { duration: 2000 });
        this.removeLocal(item.id);
        this.busy.set(null);
      },
      error: () => this.busy.set(null),
    });
  }

  /** 本地移除已处理条目；跨页回填保持每页数量 */
  private removeLocal(id: string): void {
    this.items.update((list) => list.filter((it) => it.id !== id));
    this.total.update((t) => Math.max(0, t - 1));
    if (this.items().length === 0 && this.currentPage > 0) {
      this.currentPage--;
    }
    if (this.total() === 0) {
      this.loadPage();
    }
  }

  back(): void {
    this.router.navigate(['/words']);
  }
}
