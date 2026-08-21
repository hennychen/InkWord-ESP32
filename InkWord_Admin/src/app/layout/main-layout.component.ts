import { Component, inject } from '@angular/core';
import { CommonModule } from '@angular/common';
import { RouterOutlet, RouterLink, RouterLinkActive } from '@angular/router';
import { MatSidenavModule } from '@angular/material/sidenav';
import { MatToolbarModule } from '@angular/material/toolbar';
import { MatListModule } from '@angular/material/list';
import { MatIconModule } from '@angular/material/icon';
import { MatButtonModule } from '@angular/material/button';
import { MatTooltipModule } from '@angular/material/tooltip';
import { AuthService } from '../core/services/auth.service';
import { ThemeService } from '../core/services/theme.service';

interface NavItem {
  label: string;
  icon: string;
  route: string;
}

/**
 * 主布局组件：Material sidenav + toolbar，
 * 左侧导航栏 + 顶部工具栏（含主题切换 + 登出按钮）。
 */
@Component({
  selector: 'app-main-layout',
  standalone: true,
  imports: [
    CommonModule,
    RouterOutlet,
    RouterLink,
    RouterLinkActive,
    MatSidenavModule,
    MatToolbarModule,
    MatListModule,
    MatIconModule,
    MatButtonModule,
    MatTooltipModule,
  ],
  templateUrl: './main-layout.component.html',
  styleUrl: './main-layout.component.scss',
})
export class MainLayoutComponent {
  private readonly authService = inject(AuthService);
  readonly themeService = inject(ThemeService);

  readonly navItems: NavItem[] = [
    { label: '数据看板', icon: 'dashboard', route: '/dashboard' },
    { label: '错词排行', icon: 'error_outline', route: '/wrong-top' },
    { label: '词库管理', icon: 'menu_book', route: '/words' },
    { label: '设备管理', icon: 'devices', route: '/devices' },
    { label: 'OTA 升级', icon: 'system_update', route: '/ota' },
  ];

  get username(): string | null {
    return this.authService.username();
  }

  /** 主题按钮图标：亮/暗/跟随系统三态 */
  readonly themeIcon = {
    light: 'light_mode',
    dark: 'dark_mode',
    auto: 'brightness_auto',
  } as const;

  /** 主题按钮提示（含当前生效状态） */
  get themeTooltip(): string {
    const m = this.themeService.mode();
    if (m === 'auto') {
      return `跟随系统（当前：${this.themeService.resolved() === 'dark' ? '暗色' : '亮色'}）`;
    }
    return m === 'dark' ? '暗色模式' : '亮色模式';
  }

  cycleTheme(): void {
    this.themeService.cycle();
  }

  logout(): void {
    this.authService.logout();
  }
}
