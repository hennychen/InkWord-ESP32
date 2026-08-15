import { Component, inject, signal } from '@angular/core';
import { CommonModule } from '@angular/common';
import { ReactiveFormsModule, FormBuilder, Validators } from '@angular/forms';
import { MAT_DIALOG_DATA, MatDialogRef, MatDialogModule } from '@angular/material/dialog';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatInputModule } from '@angular/material/input';
import { MatButtonModule } from '@angular/material/button';
import { MatSelectModule } from '@angular/material/select';
import { MatChipsModule, MatChipInputEvent } from '@angular/material/chips';
import { MatIconModule } from '@angular/material/icon';
import { MatSnackBar } from '@angular/material/snack-bar';
import { WordApiService } from '../../core/api/word-api.service';
import { Word } from '../../core/models/models';

/**
 * 词库编辑/新增弹窗 (A-07)：
 * 响应式表单 + 校验 + 标签 chips 输入 + 例句。
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
    MatChipsModule,
    MatIconModule,
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
  readonly tags = signal<string[]>(this.data?.tags ?? []);

  readonly form = this.fb.nonNullable.group({
    text: [this.data?.text ?? '', [Validators.required]],
    phonetic: [this.data?.phonetic ?? ''],
    definition: [this.data?.definition ?? '', [Validators.required]],
    example: [this.data?.example ?? ''],
    difficulty: [this.data?.difficulty ?? 1, [Validators.required]],
    source: [this.data?.source ?? ''],
  });

  addTag(event: MatChipInputEvent): void {
    const value = (event.value || '').trim();
    if (value) {
      this.tags.update((tags) => [...tags, value]);
    }
    event.chipInput!.clear();
  }

  removeTag(tag: string): void {
    this.tags.update((tags) => tags.filter((t) => t !== tag));
  }

  save(): void {
    if (this.form.invalid) {
      this.form.markAllAsTouched();
      return;
    }

    this.saving.set(true);
    const value = { ...this.form.getRawValue(), tags: this.tags() };

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
