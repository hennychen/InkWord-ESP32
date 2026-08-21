import { Injectable, signal, computed, effect } from '@angular/core';
import { DOCUMENT } from '@angular/common';
import { inject } from '@angular/core';

export type ThemeMode = 'light' | 'dark' | 'auto';

const THEME_KEY = 'inkword_theme';

/**
 * 主题服务 (A-11)：
 * 三态模式 —— 亮色 / 暗色 / 跟随系统（auto）。
 * auto 模式监听 prefers-color-scheme 实时切换；选择持久化至 localStorage。
 * 生效主题（resolved）为 computed：mode 取 auto 时读系统偏好。
 */
@Injectable({ providedIn: 'root' })
export class ThemeService {
  private readonly document = inject(DOCUMENT);

  /** 用户选择（含 auto） */
  readonly mode = signal<ThemeMode>(this.getStoredMode());

  /** 系统偏好（matchMedia 实时同步） */
  private readonly systemDark = signal(this.readSystemPref());

  /** 实际生效的主题（亮/暗）：mode 或（auto 时）系统偏好 */
  readonly resolved = computed<'light' | 'dark'>(() => {
    const mode = this.mode();
    return mode === 'auto' ? (this.systemDark() ? 'dark' : 'light') : mode;
  });

  constructor() {
    // 系统偏好变化时更新（auto 模式下 computed 自动重算并即时跟随）
    window
      .matchMedia('(prefers-color-scheme: dark)')
      .addEventListener('change', (e) => this.systemDark.set(e.matches));

    // 生效主题变化时应用 body class + 持久化用户选择
    effect(() => {
      this.applyTheme(this.resolved());
      localStorage.setItem(THEME_KEY, this.mode());
    });
  }

  /** 循环切换：亮 → 暗 → 自动 → 亮 */
  cycle(): void {
    this.mode.update((m) =>
      m === 'light' ? 'dark' : m === 'dark' ? 'auto' : 'light',
    );
  }

  private applyTheme(resolved: 'light' | 'dark'): void {
    const body = this.document.body;
    body.classList.toggle('dark-theme', resolved === 'dark');
    body.classList.toggle('light-theme', resolved === 'light');
    // 告知浏览器表单控件/滚动条也用对应配色
    this.document.documentElement.style.colorScheme = resolved;
  }

  private readSystemPref(): boolean {
    return window.matchMedia('(prefers-color-scheme: dark)').matches;
  }

  private getStoredMode(): ThemeMode {
    const stored = localStorage.getItem(THEME_KEY);
    // 默认跟随系统；用户手动选择后持久化覆盖
    return stored === 'light' || stored === 'dark' ? stored : 'auto';
  }
}
