import { Component, OnInit, AfterViewInit, ViewChild, inject, signal } from '@angular/core';
import { CommonModule } from '@angular/common';
import { MatTableDataSource, MatTableModule } from '@angular/material/table';
import { MatPaginator, MatPaginatorModule, PageEvent } from '@angular/material/paginator';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatInputModule } from '@angular/material/input';
import { MatButtonModule } from '@angular/material/button';
import { MatIconModule } from '@angular/material/icon';
import { MatSelectModule } from '@angular/material/select';
import { MatProgressSpinnerModule } from '@angular/material/progress-spinner';
import { MatDialog, MatDialogModule } from '@angular/material/dialog';
import { MatTooltipModule } from '@angular/material/tooltip';
import { MatSnackBar } from '@angular/material/snack-bar';
import { Router, ActivatedRoute } from '@angular/router';
import { debounceTime, distinctUntilChanged, Subject } from 'rxjs';
import { BookApiService } from '../../core/api/book-api.service';
import { BookListItem } from '../../core/models/models';
import { BookUploadComponent } from './book-upload.component';
import { BookDetailComponent } from './book-detail.component';

/**
 * 书籍列表页（阅读器后端，2026-09）：
 * 筛选栏（关键词/语言/发布态）+ 分页表格 + 上传/发布/删除/详情。
 * 发布后设备端「云端书架」可见；搜索后 URL Query 参数变化（可分享链接）。
 */
@Component({
  selector: 'app-book-list',
  standalone: true,
  imports: [
    CommonModule,
    MatTableModule,
    MatPaginatorModule,
    MatFormFieldModule,
    MatInputModule,
    MatButtonModule,
    MatIconModule,
    MatSelectModule,
    MatProgressSpinnerModule,
    MatDialogModule,
    MatTooltipModule,
  ],
  templateUrl: './book-list.component.html',
  styleUrl: './book-list.component.scss',
})
export class BookListComponent implements OnInit, AfterViewInit {
  private readonly bookApi = inject(BookApiService);
  private readonly dialog = inject(MatDialog);
  private readonly snackBar = inject(MatSnackBar);
  private readonly router = inject(Router);
  private readonly route = inject(ActivatedRoute);

  readonly loading = signal(true);
  readonly displayedColumns = ['title', 'bookKey', 'language', 'fileSize', 'downloadCount', 'published', 'actions'];

  dataSource = new MatTableDataSource<BookListItem>([]);
  total = 0;
  pageSize = 20;
  currentPage = 0;

  @ViewChild(MatPaginator) paginator!: MatPaginator;

  /** 搜索防抖 */
  private readonly searchSubject = new Subject<string>();
  keyword = '';
  /** '' = 全部；'zh'/'en' */
  languageFilter = '';
  /** '' = 全部；'published'/'draft' */
  publishedFilter = '';

  ngAfterViewInit(): void {
    this.searchSubject
      .pipe(debounceTime(400), distinctUntilChanged())
      .subscribe((term) => {
        this.keyword = term;
        this.currentPage = 0;
        // 仅改 URL，加载由 queryParams 订阅单入口触发（避免双发请求）
        this.updateQueryParams();
      });
  }

  ngOnInit(): void {
    // 从 URL Query 参数恢复状态
    this.route.queryParams.subscribe((params) => {
      if (params['keyword']) this.keyword = params['keyword'];
      if (params['language']) this.languageFilter = params['language'];
      if (params['published']) this.publishedFilter = params['published'];
      if (params['page']) this.currentPage = +params['page'] - 1;
      this.loadData();
    });
  }

  onSearch(term: string): void {
    this.searchSubject.next(term);
  }

  onLanguageChange(lang: string): void {
    this.languageFilter = lang;
    this.currentPage = 0;
    this.updateQueryParams();
  }

  onPublishedChange(state: string): void {
    this.publishedFilter = state;
    this.currentPage = 0;
    this.updateQueryParams();
  }

  onPageChange(e: PageEvent): void {
    this.currentPage = e.pageIndex;
    this.pageSize = e.pageSize;
    this.updateQueryParams();
  }

  /** 文件大小展示（KB/MB） */
  fileSizeLabel(size: number): string {
    return size > 1024 * 1024
      ? `${(size / 1024 / 1024).toFixed(1)} MB`
      : `${Math.round(size / 1024)} KB`;
  }

  private updateQueryParams(): void {
    const params: Record<string, string> = {};
    if (this.keyword) params['keyword'] = this.keyword;
    if (this.languageFilter) params['language'] = this.languageFilter;
    if (this.publishedFilter) params['published'] = this.publishedFilter;
    params['page'] = String(this.currentPage + 1);
    this.router.navigate([], {
      relativeTo: this.route,
      queryParams: params,
      replaceUrl: true,
    });
  }

  private loadData(): void {
    this.loading.set(true);
    this.bookApi
      .query({
        page: this.currentPage + 1,
        size: this.pageSize,
        keyword: this.keyword || undefined,
        language: this.languageFilter || undefined,
        published: this.publishedFilter === '' ? undefined : this.publishedFilter === 'published',
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

  /** 打开上传弹窗 */
  openUpload(): void {
    const ref = this.dialog.open(BookUploadComponent, { width: '600px' });
    ref.afterClosed().subscribe((result) => {
      if (result) this.loadData();
    });
  }

  /** 打开详情弹窗（阅读统计 + 元数据编辑） */
  openDetail(book: BookListItem): void {
    const ref = this.dialog.open(BookDetailComponent, { width: '600px', data: book });
    ref.afterClosed().subscribe((result) => {
      if (result) this.loadData();
    });
  }

  /** 发布 / 取消发布（确认框） */
  togglePublish(book: BookListItem): void {
    const action = book.published ? '取消发布' : '发布';
    if (!confirm(`确认${action}「${book.title}」吗？`)) return;
    this.bookApi.publish(book.id, !book.published).subscribe({
      next: () => {
        this.snackBar.open(`${action}成功`, '关闭', { duration: 2000 });
        this.loadData();
      },
    });
  }

  /** 删除（确认框：软删除记录 + 移除文件） */
  delete(book: BookListItem): void {
    if (!confirm(`确认删除「${book.title}」吗？书籍文件将一并移除，设备端将不可再下载。`)) return;
    this.bookApi.delete(book.id).subscribe({
      next: () => {
        this.snackBar.open('删除成功', '关闭', { duration: 2000 });
        this.loadData();
      },
    });
  }
}
