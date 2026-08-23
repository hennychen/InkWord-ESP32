import { Component, inject, signal } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { MatDialogModule, MatDialogRef } from '@angular/material/dialog';
import { MatButtonModule } from '@angular/material/button';
import { MatIconModule } from '@angular/material/icon';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatInputModule } from '@angular/material/input';
import { MatSelectModule } from '@angular/material/select';
import { MatSnackBar } from '@angular/material/snack-bar';
import { MatProgressSpinnerModule } from '@angular/material/progress-spinner';
import { WordApiService } from '../../core/api/word-api.service';

/**
 * AI 批量生成触发弹窗 (M1 路径 B，2026-08-22)：
 * 选择生成类型（分级例句/词根助记/易混辨析）+ 可选标签过滤/数量上限，
 * 提交后立即入 Hangfire 队列异步执行（进度见 /hangfire）。
 */
@Component({
  selector: 'app-ai-generate-dialog',
  standalone: true,
  imports: [
    CommonModule,
    FormsModule,
    MatDialogModule,
    MatButtonModule,
    MatIconModule,
    MatFormFieldModule,
    MatInputModule,
    MatSelectModule,
    MatProgressSpinnerModule,
  ],
  templateUrl: './ai-generate-dialog.component.html',
  styleUrl: './ai-generate-dialog.component.scss',
})
export class AiGenerateDialogComponent {
  private readonly wordApi = inject(WordApiService);
  private readonly dialogRef = inject(MatDialogRef<AiGenerateDialogComponent>);
  private readonly snackBar = inject(MatSnackBar);

  readonly submitting = signal(false);

  /** 0=分级例句 1=词根助记 2=易混辨析（后端 AiContentKind） */
  readonly kindOptions = [
    { value: 0, label: '分级例句（写入 Example）' },
    { value: 1, label: '词根助记（写入 Root）' },
    { value: 2, label: '易混辨析（仅审阅参考）' },
  ];

  kind = 0;
  tag = '';
  limit: number | null = null;

  submit(): void {
    this.submitting.set(true);
    this.wordApi
      .aiGenerate({
        kind: this.kind,
        tag: this.tag.trim() || undefined,
        limit: this.limit && this.limit > 0 ? this.limit : undefined,
      })
      .subscribe({
        next: () => {
          this.snackBar.open(
            'AI 生成任务已入队（夜间 2:00 也会自动跑批量），进度见 /hangfire',
            '关闭',
            { duration: 5000 },
          );
          this.dialogRef.close(true);
        },
        error: () => this.submitting.set(false),
      });
  }

  close(): void {
    this.dialogRef.close();
  }
}
