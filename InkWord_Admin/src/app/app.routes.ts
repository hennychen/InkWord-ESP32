import { Routes } from '@angular/router';
import { authGuard, loginGuard } from './core/guards/auth.guard';
import { MainLayoutComponent } from './layout/main-layout.component';
import { LoginComponent } from './auth/login.component';

/**
 * 路由配置 (A-02~A-10)：
 * - /auth/login  — 登录页（loginGuard：已登录则跳转 dashboard）
 * - /            — 受保护区域（authGuard），使用 MainLayoutComponent 布局
 *   Phase 2 核心页面通过 lazy-load 懒加载
 */
export const routes: Routes = [
  {
    path: 'auth/login',
    component: LoginComponent,
    canActivate: [loginGuard],
    title: '登录 — InkWord',
  },
  {
    path: '',
    component: MainLayoutComponent,
    canActivate: [authGuard],
    children: [
      { path: '', redirectTo: 'dashboard', pathMatch: 'full' },
      {
        path: 'dashboard',
        loadComponent: () =>
          import('./modules/dashboard/dashboard.component').then((m) => m.DashboardComponent),
        title: '数据看板 — InkWord',
      },
      {
        path: 'wrong-top',
        loadComponent: () =>
          import('./modules/wrongbook/wrong-top.component').then((m) => m.WrongTopComponent),
        title: '错词排行 — InkWord',
      },
      {
        path: 'words/ai-review',
        loadComponent: () =>
          import('./modules/word-management/ai-review.component').then((m) => m.AiReviewComponent),
        title: 'AI 审核 — InkWord',
      },
      {
        path: 'words',
        loadComponent: () =>
          import('./modules/word-management/word-list.component').then((m) => m.WordListComponent),
        title: '词库管理 — InkWord',
      },
      {
        path: 'devices',
        loadComponent: () =>
          import('./modules/device-management/device-list.component').then(
            (m) => m.DeviceListComponent,
          ),
        title: '设备管理 — InkWord',
      },
      {
        path: 'ota',
        loadComponent: () =>
          import('./modules/ota/ota-list.component').then((m) => m.OtaListComponent),
        title: 'OTA 升级 — InkWord',
      },
    ],
  },
  { path: '**', redirectTo: '' },
];
