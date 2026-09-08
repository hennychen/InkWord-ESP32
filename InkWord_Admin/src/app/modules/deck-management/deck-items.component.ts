import { Component, OnInit, ViewChild, inject, signal } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { HttpErrorResponse } from '@angular/common/http';
import {
  MAT_DIALOG_DATA, MatDialogModule, MatDialogRef,
} from '@angular/material/dialog';
import { MatTableDataSource, MatTableModule } from '@angular/material/table';
import { MatPaginator, MatPaginatorModule, PageEvent } from '@angular/material/paginator';
import { MatButtonModule } from '@angular/material/button';
import { MatIconModule } from '@angular/material/icon';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatInputModule } from '@angular/material/input';
import { MatProgressSpinnerModule } from '@angular/material/progress-spinner';
import { MatTooltipModule } from '@angular/material/tooltip';
import { MatSnackBar } from '@angular/material/snack-bar';
import { DeckApiService } from '../../core/api/deck-api.service';
import { DeckDetail, DeckItem, DeckListItem } from '../../core/models/models';

/** 行内编辑 / 新增共用的草稿（通用列，版式只改标签不改字段名） */
interface ItemDraft {
  front: string;
  back: string;
  phonetic: string;
  example: string;
}

/**
 * 卡组条目管理弹窗（P3，2026-09）：
 * 详情统计（学习覆盖）+ 条目分页 + 行内编辑 / 新增 / 删除。
 * 写路径后端 FillWord/NextVersion 同源：Front/Back 双写 Text/Meaning，
 * Version 接全局 max 递增 → 设备增量同步即时下发（T4.1 契约）。
 * 通用列 front/back/phonetic/example，qa/poem 版式仅标签映射同名。
 */
@Component({
  selector: 'app-deck-items',
  standalone: true,
  imports: [
    CommonModule,
    FormsModule,
    MatDialogModule,
    MatTableModule,
    MatPaginatorModule,
    MatButtonModule,
    MatIconModule,
    MatFormFieldModule,
    MatInputModule,
    MatProgressSpinnerModule,
    MatTooltipModule,
  ],
  templateUrl: './deck-items.component.html',
  styleUrl: './deck-items.component.scss',
})
export class DeckItemsComponent implements OnInit {
  private readonly deckApi = inject(DeckApiService);
  private readonly dialogRef = inject(MatDialogRef<DeckItemsComponent>);
  private readonly snackBar = inject(MatSnackBar);
  /** 模板需访问（版式标签/标题），故公开 */
  readonly deck = inject<DeckListItem>(MAT_DIALOG_DATA);

  readonly loading = signal(true);
  readonly detail = signal<DeckDetail | null>(null);
  readonly loadingItems = signal(true);
  readonly saving = signal(false);
  /** 正在行内编辑的条目 id；null = 无 */
  readonly editingId = signal<string | null>(null);
  /** 新增表单展开态 */
  readonly adding = signal(false);

  /** 编辑 / 新增草稿（ngModel 双绑） */
  draft: ItemDraft = { front: '', back: '', phonetic: '', example: '' };

  readonly displayedColumns = ['version', 'front', 'back', 'phonetic', 'state', 'actions'];
  dataSource = new MatTableDataSource<DeckItem>([]);
  total = 0;
  page = 1;
  size = 20;

  @ViewChild(MatPaginator) paginator!: MatPaginator;

  ngOnInit(): void {
    this.loadDetail();
    this.loadItems();
  }

  private loadDetail(): void {
    this.deckApi.detail(this.deck.id).subscribe({
      next: (res) => this.detail.set(res.data ?? null),
      error: () => this.detail.set(null),
    });
  }

  private loadItems(): void {
    this.loadingItems.set(true);
    this.deckApi.items(this.deck.id, this.page, this.size).subscribe({
      next: (res) => {
        const d = res.data;
        if (d) {
          this.dataSource.data = d.items;
          this.total = d.total;
        }
        this.loadingItems.set(false);
      },
      error: () => this.loadingItems.set(false),
    });
  }

  onPage(e: PageEvent): void {
    this.page = e.pageIndex + 1;
    this.size = e.pageSize;
    this.loadItems();
  }

  // ---- 版式标签映射（字段同名，仅展示文案分派） ----

  payloadLabel(): string {
    const labels: Record<string, string> = {
      'word-card': '单词卡',
      'qa-card': '问答卡',
      'poem-card': '诗文卡',
    };
    return labels[this.deck.payloadType] ?? this.deck.payloadType;
  }

  frontLabel(): string {
    switch (this.deck.payloadType) {
      case 'qa-card': return '题面';
      case 'poem-card': return '上句';
      default: return '单词（正面）';
    }
  }

  backLabel(): string {
    switch (this.deck.payloadType) {
      case 'qa-card': return '答案';
      case 'poem-card': return '下句';
      default: return '释义（背面）';
    }
  }

  exampleLabel(): string {
    switch (this.deck.payloadType) {
      case 'qa-card': return '解析';
      case 'poem-card': return '译文';
      default: return '例句';
    }
  }

  // ---- 行内编辑 ----

  startEdit(item: DeckItem): void {
    this.cancelAdd();
    this.draft = {
      front: item.front,
      back: item.back,
      phonetic: item.phonetic ?? '',
      example: item.example ?? '',
    };
    this.editingId.set(item.id);
  }

  cancelEdit(): void {
    this.editingId.set(null);
  }

  saveEdit(item: DeckItem): void {
    const front = this.draft.front.trim();
    if (!front) {
      this.snackBar.open(`${this.frontLabel()}不能为空`, '关闭', { duration: 2000 });
      return;
    }
    this.saving.set(true);
    this.deckApi.updateItem(this.deck.id, item.id, { ...this.draft, front }).subscribe({
      next: (res) => {
        this.snackBar.open(
          `已保存（v${res.data?.version ?? '??'}，设备增量同步将下发）`, '关闭', { duration: 3000 });
        this.cancelEdit();
        this.saving.set(false);
        this.loadItems();
        this.loadDetail();
      },
      error: (err: HttpErrorResponse) => {
        this.saving.set(false);
        this.showError(err);
      },
    });
  }

  // ---- 新增 ----

  startAdd(): void {
    this.cancelEdit();
    this.draft = { front: '', back: '', phonetic: '', example: '' };
    this.adding.set(true);
  }

  cancelAdd(): void {
    this.adding.set(false);
  }

  saveAdd(): void {
    const front = this.draft.front.trim();
    if (!front) {
      this.snackBar.open(`${this.frontLabel()}不能为空`, '关闭', { duration: 2000 });
      return;
    }
    this.saving.set(true);
    this.deckApi.addItem(this.deck.id, { ...this.draft, front }).subscribe({
      next: (res) => {
        this.snackBar.open(
          `已新增（v${res.data?.version ?? '??'}，设备增量同步将下发）`, '关闭', { duration: 3000 });
        this.cancelAdd();
        this.saving.set(false);
        // 新条目 CreatedAt 最新 → 跳最后一页可见（列表按录入序排）
        this.page = Math.max(1, Math.ceil((this.total + 1) / this.size));
        this.loadItems();
        this.loadDetail();
      },
      error: (err: HttpErrorResponse) => {
        this.saving.set(false);
        this.showError(err);
      },
    });
  }

  // ---- 删除 ----

  remove(item: DeckItem): void {
    if (!confirm(`确认删除条目「${item.front}」吗？注意：已同步到设备的条目不会自动消失（需 App LAN 重推或重新导出词库覆盖）。`)) return;
    this.saving.set(true);
    this.deckApi.deleteItem(this.deck.id, item.id).subscribe({
      next: () => {
        this.snackBar.open('已删除（归档；新设备/重新导出不再到，已同步设备需 LAN 重推覆盖）', '关闭', { duration: 4000 });
        this.saving.set(false);
        // 删除后总量 -1，末页回收防空页误显「暂无条目」
        this.page = Math.min(this.page, Math.max(1, Math.ceil((this.total - 1) / this.size)));
        this.loadItems();
        this.loadDetail();
      },
      error: (err: HttpErrorResponse) => {
        this.saving.set(false);
        this.showError(err);
      },
    });
  }

  /** 服务端错误文案（409 uq 冲突等 ApiResponse.message 透传） */
  private showError(err: HttpErrorResponse): void {
    const msg = (err.error as { message?: string } | null)?.message ?? '操作失败';
    this.snackBar.open(msg, '关闭', { duration: 3000 });
  }

  close(): void {
    this.dialogRef.close(true);
  }
}
