import { Component, OnInit, AfterViewInit, ViewChild, inject, signal } from '@angular/core';
import { CommonModule } from '@angular/common';
import { MatTableDataSource, MatTableModule } from '@angular/material/table';
import { MatPaginator, MatPaginatorModule, PageEvent } from '@angular/material/paginator';
import { MatSort, MatSortModule } from '@angular/material/sort';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatInputModule } from '@angular/material/input';
import { MatButtonModule } from '@angular/material/button';
import { MatIconModule } from '@angular/material/icon';
import { MatSelectModule } from '@angular/material/select';
import { MatProgressSpinnerModule } from '@angular/material/progress-spinner';
import { MatBadgeModule } from '@angular/material/badge';
import { MatDialog, MatDialogModule } from '@angular/material/dialog';
import { MatSnackBar } from '@angular/material/snack-bar';
import { Router, ActivatedRoute } from '@angular/router';
import { debounceTime, distinctUntilChanged, Subject } from 'rxjs';
import { WordApiService } from '../../core/api/word-api.service';
import { Word } from '../../core/models/models';
import { WordEditComponent } from './word-edit.component';
import { WordImportComponent } from './word-import.component';
import { AiGenerateDialogComponent } from './ai-generate-dialog.component';
import { DeckGenerateDialogComponent } from './deck-generate-dialog.component';

/**
 * 词库列表页 (A-06)：
 * MatTable + 分页 + 搜索框 + 标签过滤。
 * 搜索后 URL Query 参数变化（可分享链接）。
 */
@Component({
  selector: 'app-word-list',
  standalone: true,
  imports: [
    CommonModule,
    MatTableModule,
    MatPaginatorModule,
    MatSortModule,
    MatFormFieldModule,
    MatInputModule,
    MatButtonModule,
    MatIconModule,
    MatSelectModule,
    MatProgressSpinnerModule,
    MatBadgeModule,
    MatDialogModule,
  ],
  templateUrl: './word-list.component.html',
  styleUrl: './word-list.component.scss',
})
export class WordListComponent implements OnInit, AfterViewInit {
  private readonly wordApi = inject(WordApiService);
  private readonly dialog = inject(MatDialog);
  private readonly snackBar = inject(MatSnackBar);
  private readonly router = inject(Router);
  private readonly route = inject(ActivatedRoute);

  readonly loading = signal(true);
  /** AI 待审数量（角标；M1 路径 B） */
  readonly aiPending = signal(0);
  readonly displayedColumns = ['text', 'phonetic', 'meaning', 'tag', 'difficulty', 'aiStatus', 'actions'];

  dataSource = new MatTableDataSource<Word>([]);
  total = 0;
  pageSize = 20;
  currentPage = 0;

  @ViewChild(MatPaginator) paginator!: MatPaginator;
  @ViewChild(MatSort) sort!: MatSort;

  /** 搜索防抖 */
  private readonly searchSubject = new Subject<string>();
  keyword = '';
  tagFilter = '';
  difficultyFilter: number | null = null;

  readonly difficultyOptions = [
    { value: 1, label: '简单' },
    { value: 2, label: '中等' },
    { value: 3, label: '困难' },
  ];

  ngAfterViewInit(): void {
    this.dataSource.sort = this.sort;

    // 搜索输入防抖
    this.searchSubject
      .pipe(debounceTime(400), distinctUntilChanged())
      .subscribe((term) => {
        this.keyword = term;
        this.currentPage = 0;
        this.updateQueryParams();
        this.loadData();
      });
  }

  ngOnInit(): void {
    // AI 待审数量（角标展示，失败静默）
    this.wordApi.aiPendingCount().subscribe({
      next: (res) => this.aiPending.set(res.data?.count ?? 0),
    });

    // 从 URL Query 参数恢复状态
    this.route.queryParams.subscribe((params) => {
      if (params['keyword']) this.keyword = params['keyword'];
      if (params['tag']) this.tagFilter = params['tag'];
      if (params['page']) this.currentPage = +params['page'] - 1;
      this.loadData();
    });
  }

  onSearch(term: string): void {
    this.searchSubject.next(term);
  }

  /** AI 状态徽标文本（0 不展示） */
  aiStatusBadge(word: Word): string {
    switch (word.aiStatus) {
      case 1: return '待审';
      case 2: return '已应用';
      case 3: return '失败';
      default: return '';
    }
  }

  /** 复位生成失败词（ai-reject 对 AiStatus=3 亦复位，重入生成队列） */
  resetAiStatus(word: Word): void {
    this.wordApi.aiReject(word.id).subscribe({
      next: () => {
        this.snackBar.open(`已复位「${word.text}」待重新生成`, '关闭', { duration: 2000 });
        this.loadData();
      },
      error: () => this.snackBar.open('复位失败', '关闭', { duration: 2000 }),
    });
  }

  onTagFilterChange(tag: string): void {
    this.tagFilter = tag;
    this.currentPage = 0;
    this.updateQueryParams();
    this.loadData();
  }

  onPageChange(e: PageEvent): void {
    this.currentPage = e.pageIndex;
    this.pageSize = e.pageSize;
    this.updateQueryParams();
    this.loadData();
  }

  private updateQueryParams(): void {
    const params: Record<string, string> = {};
    if (this.keyword) params['keyword'] = this.keyword;
    if (this.tagFilter) params['tag'] = this.tagFilter;
    params['page'] = String(this.currentPage + 1);
    this.router.navigate([], {
      relativeTo: this.route,
      queryParams: params,
      replaceUrl: true,
    });
  }

  private loadData(): void {
    this.loading.set(true);
    this.wordApi
      .query({
        page: this.currentPage + 1,
        size: this.pageSize,
        keyword: this.keyword || undefined,
        tag: this.tagFilter || undefined,
        difficulty: this.difficultyFilter ?? undefined,
      })
      .subscribe({
        next: (res) => {
          const result = res.data;
          if (result) {
            this.dataSource.data = result.items;
            this.total = result.total;
          }
          this.loading.set(false);
        },
        error: () => this.loading.set(false),
      });
  }

  /** 打开新增弹窗 */
  openCreate(): void {
    const ref = this.dialog.open(WordEditComponent, { width: '600px', data: null });
    ref.afterClosed().subscribe((result) => {
      if (result) this.loadData();
    });
  }

  /** 打开编辑弹窗 */
  openEdit(word: Word): void {
    const ref = this.dialog.open(WordEditComponent, { width: '600px', data: word });
    ref.afterClosed().subscribe((result) => {
      if (result) this.loadData();
    });
  }

  /** 删除确认 */
  delete(word: Word): void {
    if (!confirm(`确认删除单词 "${word.text}" 吗？`)) return;
    this.wordApi.delete(word.id).subscribe({
      next: () => {
        this.snackBar.open('删除成功', '关闭', { duration: 2000 });
        this.loadData();
      },
    });
  }

  /** 打开导入弹窗 */
  openImport(): void {
    const ref = this.dialog.open(WordImportComponent, { width: '600px' });
    ref.afterClosed().subscribe((result) => {
      if (result) this.loadData();
    });
  }

  /** AI 批量生成触发（M1 路径 B：入 Hangfire 队列，结果去 AI 审核台比对） */
  openAiGenerate(): void {
    this.dialog.open(AiGenerateDialogComponent, { width: '520px' });
  }

  /** T5.4 AI 卡组生成：素材→占位待审条目（审核台通过后 Version++ 下发） */
  openDeckGenerate(): void {
    this.dialog.open(DeckGenerateDialogComponent, { width: '560px' });
  }

  /** 跳转 AI 审核台（待审建议比对） */
  goAiReview(): void {
    this.router.navigate(['/words/ai-review']);
  }

  /** 导出设备词库 words.json（含 cloudId）：下载后拷入 SD 卡，
   * 设端据 cloudId 上报评分/收藏（P2 云端闭环） */
  exportLibrary(): void {
    this.wordApi.exportDeviceLibrary().subscribe({
      next: (blob) => {
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = 'words.json';
        a.click();
        URL.revokeObjectURL(url);
        this.snackBar.open('已导出 words.json，拷入 SD 卡根目录后重启设备', '关闭', { duration: 5000 });
      },
      error: () => this.snackBar.open('导出失败', '关闭', { duration: 2000 }),
    });
  }
}
