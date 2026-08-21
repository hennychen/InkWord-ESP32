import { Component, inject, signal } from '@angular/core';
import { CommonModule } from '@angular/common';
import { ReactiveFormsModule, FormBuilder, Validators } from '@angular/forms';
import { MAT_DIALOG_DATA, MatDialogRef, MatDialogModule } from '@angular/material/dialog';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatInputModule } from '@angular/material/input';
import { MatButtonModule } from '@angular/material/button';
import { MatSelectModule } from '@angular/material/select';
import { MatSnackBar } from '@angular/material/snack-bar';
import { WordApiService } from '../../core/api/word-api.service';
import { Word } from '../../core/models/models';

/**
 * 词库编辑/新增弹窗 (A-07)：
 * 响应式表单 + 校验 + 词库扩展四字段（root/inflections/source/grade，
 * 2026-08-20）；tag 为单文本（后端 string，历史 chips[] 错位已修正）。
 */
@Component({
  selector: 'app-word-edit',
  standalone: true,
  imports: [
    CommonModule,
    ReactiveFormsModule,
    MatDialogModule,
    MatFormFieldModule,
    MatInputModule,
    MatButtonModule,
    MatSelectModule,
  ],
  templateUrl: './word-edit.component.html',
  styleUrl: './word-edit.component.scss',
})
export class WordEditComponent {
  private readonly fb = inject(FormBuilder);
  private readonly wordApi = inject(WordApiService);
  private readonly dialogRef = inject(MatDialogRef<WordEditComponent>);
  private readonly snackBar = inject(MatSnackBar);
  private readonly data = inject<Word | null>(MAT_DIALOG_DATA, { optional: true });

  readonly isEdit = signal(!!this.data);
  readonly saving = signal(false);

  readonly form = this.fb.nonNullable.group({
    text: [this.data?.text ?? '', [Validators.required]],
    phonetic: [this.data?.phonetic ?? ''],
    meaning: [this.data?.meaning ?? '', [Validators.required]],
    example: [this.data?.example ?? ''],
    audio: [this.data?.audio ?? ''],
    tag: [this.data?.tag ?? ''],
    difficulty: [this.data?.difficulty ?? 1, [Validators.required]],
    root: [this.data?.root ?? ''],
    inflections: [this.data?.inflections ?? ''],
    source: [this.data?.source ?? ''],
    grade: [this.data?.grade ?? ''],
  });

  save(): void {
    if (this.form.invalid) {
      this.form.markAllAsTouched();
      return;
    }

    this.saving.set(true);
    const value = this.form.getRawValue();

    const request = this.isEdit()
      ? this.wordApi.update(this.data!.id, { id: this.data!.id, ...value })
      : this.wordApi.create(value);

    request.subscribe({
      next: () => {
        this.snackBar.open(this.isEdit() ? '修改成功' : '新增成功', '关闭', { duration: 2000 });
        this.dialogRef.close(true);
      },
      error: () => this.saving.set(false),
    });
  }

  close(): void {
    this.dialogRef.close();
  }
}
