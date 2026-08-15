import { Injectable, signal, effect } from '@angular/core';
import { DOCUMENT } from '@angular/common';
import { inject } from '@angular/core';

export type ThemeMode = 'light' | 'dark';

const THEME_KEY = 'inkword_theme';

/**
 * 主题服务 (A-11)：
 * 管理亮/暗模式切换，通过给 <body> 添加 CSS class 实现。
 * 选择持久化至 localStorage。
 */
@Injectable({ providedIn: 'root' })
export class ThemeService {
  private readonly document = inject(DOCUMENT);

  /** 当前主题 */
  readonly mode = signal<ThemeMode>(this.getStoredTheme());

  constructor() {
    // 主题变化时自动应用
    effect(() => {
      const mode = this.mode();
      this.applyTheme(mode);
      localStorage.setItem(THEME_KEY, mode);
    });
  }

  /** 切换主题 */
  toggle(): void {
    this.mode.update((m) => (m === 'light' ? 'dark' : 'light'));
  }

  private applyTheme(mode: ThemeMode): void {
    const body = this.document.body;
    if (mode === 'dark') {
      body.classList.add('dark-theme');
      body.classList.remove('light-theme');
    } else {
      body.classList.add('light-theme');
      body.classList.remove('dark-theme');
    }
  }

  private getStoredTheme(): ThemeMode {
    const stored = localStorage.getItem(THEME_KEY);
    return stored === 'dark' ? 'dark' : 'light';
  }
}
