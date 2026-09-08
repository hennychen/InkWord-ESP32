import { Component, inject, signal } from '@angular/core';
import { CommonModule } from '@angular/common';
import { ReactiveFormsModule, FormBuilder, Validators } from '@angular/forms';
import { MAT_DIALOG_DATA, MatDialogRef, MatDialogModule } from '@angular/material/dialog';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatInputModule } from '@angular/material/input';
import { MatButtonModule } from '@angular/material/button';
import { MatSelectModule } from '@angular/material/select';
import { MatIconModule } from '@angular/material/icon';
import { MatSnackBar } from '@angular/material/snack-bar';
import { BookApiService } from '../../core/api/book-api.service';

/** 支持的书籍格式（后端 10MB 上限同口径） */
const MAX_SIZE = 10 * 1024 * 1024;

/**
 * 书籍上传弹窗（阅读器后端，2026-09）：
 * 文件选择（TXT/MD/HTML ≤10MB，书名默认取文件名）+ 元数据表单。
 * 上传后为草稿态，需在列表手动发布。
 */
@Component({
  selector: 'app-book-upload',
  standalone: true,
  imports: [
    CommonModule,
    ReactiveFormsModule,
    MatDialogModule,
    MatFormFieldModule,
    MatInputModule,
    MatButtonModule,
    MatSelectModule,
    MatIconModule,
  ],
  templateUrl: './book-upload.component.html',
  styleUrl: './book-upload.component.scss',
})
export class BookUploadComponent {
  private readonly fb = inject(FormBuilder);
  private readonly bookApi = inject(BookApiService);
  private readonly dialogRef = inject(MatDialogRef<BookUploadComponent>);
  private readonly snackBar = inject(MatSnackBar);

  readonly saving = signal(false);
  /** 已选文件（选择后书名自动填充文件名） */
  readonly file = signal<File | null>(null);
  readonly fileError = signal('');

  readonly form = this.fb.nonNullable.group({
    title: ['', [Validators.required]],
    author: [''],
    language: ['zh', [Validators.required]],
    tags: [''],
    description: [''],
  });

  /** 选择文件：校验格式 + 大小，书名缺省回填 */
  onFileSelected(event: Event): void {
    const input = event.target as HTMLInputElement;
    const selected = input.files?.[0] ?? null;
    this.fileError.set('');

    if (!selected) {
      this.file.set(null);
      return;
    }
    const ext = selected.name.split('.').pop()?.toLowerCase() ?? '';
    if (!['txt', 'md', 'htm', 'html'].includes(ext)) {
      this.fileError.set('仅支持 TXT / MD / HTML 格式');
      input.value = '';
      this.file.set(null);
      return;
    }
    if (selected.size > MAX_SIZE) {
      this.fileError.set('文件不能超过 10MB');
      input.value = '';
      this.file.set(null);
      return;
    }

    this.file.set(selected);
    if (!this.form.controls.title.value) {
      const baseName = selected.name.replace(/\.[^.]+$/, '');
      this.form.controls.title.setValue(baseName);
    }
  }

  /** 文件大小展示（KB/MB） */
  fileSizeLabel(size: number): string {
    return size > 1024 * 1024
      ? `${(size / 1024 / 1024).toFixed(1)} MB`
      : `${Math.round(size / 1024)} KB`;
  }

  upload(): void {
    const file = this.file();
    if (!file) {
      this.fileError.set('请先选择书籍文件');
      return;
    }
    if (this.form.invalid) {
      this.form.markAllAsTouched();
      return;
    }

    this.saving.set(true);
    this.bookApi.upload(file, this.form.getRawValue()).subscribe({
      next: (res) => {
        const bookKey = res.data?.bookKey ?? '';
        this.snackBar.open(`上传成功（BookKey: ${bookKey}），请在列表中发布`, '关闭', { duration: 4000 });
        this.dialogRef.close(true);
      },
      error: () => this.saving.set(false),
    });
  }

  close(): void {
    this.dialogRef.close();
  }
}
