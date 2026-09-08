import { Component, inject, signal } from '@angular/core';
import { CommonModule } from '@angular/common';
import { MAT_DIALOG_DATA, MatDialogModule, MatDialogRef } from '@angular/material/dialog';
import { MatButtonModule } from '@angular/material/button';
import { MatIconModule } from '@angular/material/icon';
import { MatProgressSpinnerModule } from '@angular/material/progress-spinner';
import { MatSnackBar } from '@angular/material/snack-bar';
import { DeckApiService } from '../../core/api/deck-api.service';
import { DeckListItem } from '../../core/models/models';

/**
 * 卡组字符集查看弹窗（P3，2026-09，基于 v1.4 T4.5 charset 端点）：
 * 拉取该卡组全部条目可渲染九列的去重字符流，核对生僻字覆盖；
 * 可复制 / 下载 .txt 喂 tools/gen_cjk_font.swift --subset 产子集字库
 * （deck_<code>.bin 拷入 SD /fonts/，固件 cjk_font_sd 级联查找）。
 */
@Component({
  selector: 'app-deck-charset',
  standalone: true,
  imports: [
    CommonModule,
    MatDialogModule,
    MatButtonModule,
    MatIconModule,
    MatProgressSpinnerModule,
  ],
  templateUrl: './deck-charset.component.html',
  styleUrl: './deck-charset.component.scss',
})
export class DeckCharsetComponent {
  private readonly deckApi = inject(DeckApiService);
  private readonly dialogRef = inject(MatDialogRef<DeckCharsetComponent>);
  private readonly snackBar = inject(MatSnackBar);
  /** 模板需访问（标题/文件名），故公开 */
  readonly deck = inject<DeckListItem>(MAT_DIALOG_DATA);

  readonly loading = signal(true);
  readonly failed = signal(false);
  readonly chars = signal('');

  constructor() {
    this.deckApi.charsetText(this.deck.code).subscribe({
      next: (text) => {
        this.chars.set(text);
        this.loading.set(false);
      },
      error: () => {
        this.failed.set(true);
        this.loading.set(false);
      },
    });
  }

  /** 复制字符流（剪贴板） */
  copy(): void {
    navigator.clipboard.writeText(this.chars()).then(
      () => this.snackBar.open('已复制到剪贴板', '关闭', { duration: 2000 }),
      () => this.snackBar.open('复制失败，请手动选择复制', '关闭', { duration: 2000 }),
    );
  }

  /** 下载 .txt（喂 gen_cjk_font.swift --subset） */
  download(): void {
    const blob = new Blob([this.chars()], { type: 'text/plain;charset=utf-8' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `deck_${this.deck.code}_charset.txt`;
    a.click();
    URL.revokeObjectURL(url);
  }

  close(): void {
    this.dialogRef.close();
  }
}
