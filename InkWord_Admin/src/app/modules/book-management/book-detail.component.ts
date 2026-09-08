import { Component, inject, signal } from '@angular/core';
import { CommonModule } from '@angular/common';
import { ReactiveFormsModule, FormBuilder, Validators } from '@angular/forms';
import { MAT_DIALOG_DATA, MatDialogRef, MatDialogModule } from '@angular/material/dialog';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatInputModule } from '@angular/material/input';
import { MatButtonModule } from '@angular/material/button';
import { MatSelectModule } from '@angular/material/select';
import { MatProgressSpinnerModule } from '@angular/material/progress-spinner';
import { MatSnackBar } from '@angular/material/snack-bar';
import { BookApiService } from '../../core/api/book-api.service';
import { BookDetail, BookListItem } from '../../core/models/models';

/**
 * 书籍详情弹窗（阅读器后端，2026-09）：
 * 阅读统计（读者数 / 平均进度 / 下载次数）+ 元数据编辑（BookUpdateDto）。
 */
@Component({
  selector: 'app-book-detail',
  standalone: true,
  imports: [
    CommonModule,
    ReactiveFormsModule,
    MatDialogModule,
    MatFormFieldModule,
    MatInputModule,
    MatButtonModule,
    MatSelectModule,
    MatProgressSpinnerModule,
  ],
  templateUrl: './book-detail.component.html',
  styleUrl: './book-detail.component.scss',
})
export class BookDetailComponent {
  private readonly fb = inject(FormBuilder);
  private readonly bookApi = inject(BookApiService);
  private readonly dialogRef = inject(MatDialogRef<BookDetailComponent>);
  private readonly snackBar = inject(MatSnackBar);
  private readonly book = inject<BookListItem>(MAT_DIALOG_DATA);

  readonly loading = signal(true);
  readonly saving = signal(false);
  readonly detail = signal<BookDetail | null>(null);

  readonly form = this.fb.nonNullable.group({
    title: ['', [Validators.required]],
    author: [''],
    language: ['zh', [Validators.required]],
    tags: [''],
    description: [''],
  });

  constructor() {
    this.bookApi.detail(this.book.id).subscribe({
      next: (res) => {
        const d = res.data ?? null;
        this.detail.set(d);
        if (d) {
          this.form.patchValue({
            title: d.title,
            author: d.author ?? '',
            language: d.language,
            tags: d.tags ?? '',
            description: d.description ?? '',
          });
        }
        this.loading.set(false);
      },
      error: () => this.loading.set(false),
    });
  }

  /** 文件大小展示（KB/MB） */
  fileSizeLabel(size: number): string {
    return size > 1024 * 1024
      ? `${(size / 1024 / 1024).toFixed(1)} MB`
      : `${Math.round(size / 1024)} KB`;
  }

  save(): void {
    if (this.form.invalid) {
      this.form.markAllAsTouched();
      return;
    }

    this.saving.set(true);
    this.bookApi.update(this.book.id, this.form.getRawValue()).subscribe({
      next: () => {
        this.snackBar.open('元数据已保存', '关闭', { duration: 2000 });
        this.dialogRef.close(true);
      },
      error: () => this.saving.set(false),
    });
  }

  close(): void {
    this.dialogRef.close();
  }
}
