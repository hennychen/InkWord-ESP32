import { Component, OnInit, inject, signal } from '@angular/core';
import { CommonModule, DatePipe } from '@angular/common';
import { MatTableDataSource, MatTableModule } from '@angular/material/table';
import { MatButtonModule } from '@angular/material/button';
import { MatIconModule } from '@angular/material/icon';
import { MatProgressSpinnerModule } from '@angular/material/progress-spinner';
import { MatDialog, MatDialogModule } from '@angular/material/dialog';
import { MatTooltipModule } from '@angular/material/tooltip';
import { MatSnackBar } from '@angular/material/snack-bar';
import { ActivatedRoute, Router } from '@angular/router';
import { DeckApiService } from '../../core/api/deck-api.service';
import { AdminSubjectItem, DeckListItem } from '../../core/models/models';
import { DeckItemsComponent } from './deck-items.component';
import { DeckCharsetComponent } from './deck-charset.component';
import { DeckGenerateDialogComponent } from './deck-generate-dialog.component';

/**
 * 卡组管理列表页（P3，2026-09）：
 * 科目 chip 侧栏过滤（每科目卡组计数）+ Deck 表（Code/名称/科目/版式/
 * 条目数/共享/Owner）+ 条目管理弹窗 + 字符集核对 + AI 生成入口
 * （自词库页迁入）。后端 List 全量返回，前端本地无分页。
 */
@Component({
  selector: 'app-deck-list',
  standalone: true,
  imports: [
    CommonModule,
    DatePipe,
    MatTableModule,
    MatButtonModule,
    MatIconModule,
    MatProgressSpinnerModule,
    MatDialogModule,
    MatTooltipModule,
  ],
  templateUrl: './deck-list.component.html',
  styleUrl: './deck-list.component.scss',
})
export class DeckListComponent implements OnInit {
  private readonly deckApi = inject(DeckApiService);
  private readonly dialog = inject(MatDialog);
  private readonly snackBar = inject(MatSnackBar);
  private readonly router = inject(Router);
  private readonly route = inject(ActivatedRoute);

  readonly loading = signal(true);
  readonly subjects = signal<AdminSubjectItem[]>([]);
  /** '' = 全部；否则科目 Code */
  subjectFilter = '';

  readonly displayedColumns = [
    'code', 'name', 'subject', 'payloadType', 'itemCount', 'maxVersion', 'shared', 'owner', 'updatedAt', 'actions',
  ];
  dataSource = new MatTableDataSource<DeckListItem>([]);

  /** 版式中文标签（与 AI 生成弹窗一致；未知版式回退原码） */
  payloadLabel(type: string): string {
    const labels: Record<string, string> = {
      'word-card': '单词卡',
      'qa-card': '问答卡',
      'poem-card': '诗文卡',
    };
    return labels[type] ?? type;
  }

  ngOnInit(): void {
    // URL ?subject= 恢复科目过滤（可分享链接）
    this.route.queryParams.subscribe((params) => {
      this.subjectFilter = params['subject'] ?? '';
      this.loadSubjects();
      this.loadDecks();
    });
  }

  private loadSubjects(): void {
    this.deckApi.subjects().subscribe({
      next: (res) => this.subjects.set(res.data ?? []),
      error: () => this.subjects.set([]),
    });
  }

  private loadDecks(): void {
    this.loading.set(true);
    this.deckApi.decks(this.subjectFilter || undefined).subscribe({
      next: (res) => {
        this.dataSource.data = res.data ?? [];
        this.loading.set(false);
      },
      error: () => {
        this.snackBar.open('卡组列表加载失败', '关闭', { duration: 2000 });
        this.loading.set(false);
      },
    });
  }

  /** 科目 chip 过滤（'' = 全部） */
  filterBy(code: string): void {
    if (this.subjectFilter === code) return;
    this.router.navigate([], {
      relativeTo: this.route,
      queryParams: code ? { subject: code } : {},
      replaceUrl: true,
    });
  }

  /** 条目管理弹窗（分页 + 行内编辑 + 新增/删除） */
  openItems(deck: DeckListItem): void {
    const ref = this.dialog.open(DeckItemsComponent, {
      width: '1080px',
      maxWidth: '96vw',
      data: deck,
    });
    ref.afterClosed().subscribe(() => this.loadDecks());
  }

  /** 字符集核对弹窗（生僻字覆盖，喂字库子集流程） */
  openCharset(deck: DeckListItem): void {
    this.dialog.open(DeckCharsetComponent, { width: '680px', data: deck });
  }

  /** AI 生成卡组（自词库页迁入的入口）：素材 → 占位待审 → 审核台下发 */
  openGenerate(): void {
    const ref = this.dialog.open(DeckGenerateDialogComponent, { width: '560px' });
    ref.afterClosed().subscribe((result) => {
      if (result) this.loadDecks();
    });
  }

  refresh(): void {
    this.loadSubjects();
    this.loadDecks();
  }
}
