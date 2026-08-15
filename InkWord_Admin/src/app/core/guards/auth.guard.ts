import { inject } from '@angular/core';
import { CanActivateFn, Router } from '@angular/router';
import { TokenService } from '../services/token.service';

/**
 * 路由守卫 (A-02)：检查用户是否已登录。
 * 未登录时重定向到 /auth/login，并记录 return URL。
 * 使用 Angular 17 函数式 Guard。
 */
export const authGuard: CanActivateFn = (route, state) => {
  const tokenService = inject(TokenService);
  const router = inject(Router);

  if (tokenService.isAuthenticated()) {
    return true;
  }

  router.navigate(['/auth/login'], {
    queryParams: { returnUrl: state.url },
  });
  return false;
};

/**
 * 反向守卫：已登录用户访问登录页时自动跳转 dashboard。
 */
export const loginGuard: CanActivateFn = () => {
  const tokenService = inject(TokenService);
  const router = inject(Router);

  if (tokenService.isAuthenticated()) {
    router.navigate(['/dashboard']);
    return false;
  }
  return true;
};
