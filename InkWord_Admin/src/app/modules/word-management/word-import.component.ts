import { Component, inject, signal } from '@angular/core';
import { CommonModule } from '@angular/common';
import { MatDialogModule, MatDialogRef } from '@angular/material/dialog';
import { MatButtonModule } from '@angular/material/button';
import { MatIconModule } from '@angular/material/icon';
import { MatProgressBarModule } from '@angular/material/progress-bar';
import { MatSnackBar } from '@angular/material/snack-bar';
import { WordApiService } from '../../core/api/word-api.service';

/**
 * CSV 批量导入弹窗 (A-08)：
 * 拖拽/点击上传 CSV 文件，提交后显示进度条，完成弹窗总结。
 */
@Component({
  selector: 'app-word-import',
  standalone: true,
  imports: [
    CommonModule,
    MatDialogModule,
    MatButtonModule,
    MatIconModule,
    MatProgressBarModule,
  ],
  templateUrl: './word-import.component.html',
  styleUrl: './word-import.component.scss',
})
export class WordImportComponent {
  private readonly wordApi = inject(WordApiService);
  private readonly dialogRef = inject(MatDialogRef<WordImportComponent>);
  private readonly snackBar = inject(MatSnackBar);

  readonly selectedFile = signal<File | null>(null);
  readonly uploading = signal(false);
  readonly progress = signal(0);
  readonly dragOver = signal(false);

  onFileSelected(event: Event): void {
    const input = event.target as HTMLInputElement;
    if (input.files?.length) {
      this.selectedFile.set(input.files[0]);
    }
  }

  onDrop(event: DragEvent): void {
    event.preventDefault();
    this.dragOver.set(false);
    if (event.dataTransfer?.files.length) {
      this.selectedFile.set(event.dataTransfer.files[0]);
    }
  }

  onDragOver(event: DragEvent): void {
    event.preventDefault();
    this.dragOver.set(true);
  }

  onDragLeave(): void {
    this.dragOver.set(false);
  }

  upload(): void {
    const file = this.selectedFile();
    if (!file) return;

    this.uploading.set(true);
    this.progress.set(10);

    // 模拟进度更新
    const timer = setInterval(() => {
      this.progress.update((p) => Math.min(p + 15, 90));
    }, 300);

    this.wordApi.importCsv(file).subscribe({
      next: (res) => {
        clearInterval(timer);
        this.progress.set(100);
        const result = res.data;
        const msg = result
          ? `导入完成：成功 ${result.success} 条，失败 ${result.failed} 条`
          : '导入完成';
        this.snackBar.open(msg, '关闭', { duration: 4000 });
        this.uploading.set(false);
        setTimeout(() => this.dialogRef.close(true), 500);
      },
      error: () => {
        clearInterval(timer);
        this.uploading.set(false);
        this.progress.set(0);
      },
    });
  }

  close(): void {
    this.dialogRef.close();
  }
}
