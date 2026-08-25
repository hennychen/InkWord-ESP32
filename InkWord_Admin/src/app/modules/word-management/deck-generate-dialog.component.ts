import { Component, OnInit, inject, signal } from '@angular/core';
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
import { AdminDeckInfo } from '../../core/models/models';

/**
 * AI 卡组生成弹窗（v1.5 T5.4，2026-08-25）：
 * 选目标卡组（版式决定 prompt 模板与素材形态）+ 粘贴素材（课文/知识点
 * 清单）+ 条数上限 → Hangfire 异步生成占位待审条目（Version=0 不下发），
 * AI 审核台通过后 Version++ 增量下发（设备零改动红线）。
 */
@Component({
  selector: 'app-deck-generate-dialog',
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
  templateUrl: './deck-generate-dialog.component.html',
  styleUrl: './deck-generate-dialog.component.scss',
})
export class DeckGenerateDialogComponent implements OnInit {
  private readonly wordApi = inject(WordApiService);
  private readonly dialogRef = inject(MatDialogRef<DeckGenerateDialogComponent>);
  private readonly snackBar = inject(MatSnackBar);

  readonly loading = signal(true);
  readonly submitting = signal(false);
  readonly decks = signal<AdminDeckInfo[]>([]);

  deckId: string | null = null;
  source = '';
  limit = 10;
  readonly maxLimit = 40;

  /** 版式中文标签（与后端 prompt 模板一一对应；未知版式回退原码） */
  payloadLabel(type: string): string {
    return this.payloadLabels[type] ?? type;
  }

  readonly payloadLabels: Record<string, string> = {
    'word-card': '单词卡',
    'qa-card': '问答卡',
    'poem-card': '诗文卡',
  };

  /** 选中卡组的素材输入提示（按版式分派） */
  sourceHint(deck: AdminDeckInfo | undefined): string {
    switch (deck?.payloadType) {
      case 'poem-card':
        return '粘贴课文/古诗文全文——AI 挑选名篇名句生成「上句→下句」默写卡（附拼音与译文）';
      case 'qa-card':
        return '粘贴知识点素材（课堂笔记/考点清单）——AI 转成「题面→答案」问答卡';
      default:
        return '粘贴单词/词条清单——AI 生成单词卡（释义/音标/例句）';
    }
  }

  selectedDeck(): AdminDeckInfo | undefined {
    return this.decks().find((d) => d.id === this.deckId);
  }

  ngOnInit(): void {
    this.wordApi.adminDecks().subscribe({
      next: (res) => {
        this.decks.set(res.data ?? []);
        this.deckId = this.decks()[0]?.id ?? null;
        this.loading.set(false);
      },
      error: () => this.loading.set(false),
    });
  }

  submit(): void {
    const deck = this.selectedDeck();
    const source = this.source.trim();
    if (!deck || !source) return;
    this.submitting.set(true);
    this.wordApi
      .deckAiGenerate(deck.id, {
        source,
        limit: Math.min(Math.max(this.limit, 1), this.maxLimit),
      })
      .subscribe({
        next: () => {
          this.snackBar.open(
            `「${deck.name}」AI 生成任务已入队，完成后到 AI 审核台逐条比对（通过才下发设备）`,
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
