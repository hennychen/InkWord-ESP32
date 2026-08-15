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
    { label: '词库管理', icon: 'menu_book', route: '/words' },
    { label: '设备管理', icon: 'devices', route: '/devices' },
    { label: 'OTA 升级', icon: 'system_update', route: '/ota' },
  ];

  get username(): string | null {
    return this.authService.username();
  }

  toggleTheme(): void {
    this.themeService.toggle();
  }

  logout(): void {
    this.authService.logout();
  }
}
