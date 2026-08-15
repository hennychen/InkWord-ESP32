import { HttpInterceptorFn, HttpErrorResponse } from '@angular/common/http';
import { inject } from '@angular/core';
import { Router } from '@angular/router';
import { catchError, throwError } from 'rxjs';
import { MatSnackBar } from '@angular/material/snack-bar';
import { TokenService } from '../services/token.service';

/**
 * 全局错误拦截器 (A-04)：统一 catchError 处理。
 * - 401 → 清除 Token 并跳转登录页
 * - 其他 → 弹出 MatSnackBar 错误提示
 */
export const errorInterceptor: HttpInterceptorFn = (req, next) => {
  const router = inject(Router);
  const snackBar = inject(MatSnackBar);
  const tokenService = inject(TokenService);

  return next(req).pipe(
    catchError((error: HttpErrorResponse) => {
      let message = '网络错误，请稍后重试';

      if (error.status === 0) {
        message = '无法连接服务器，请检查网络';
      } else if (error.status === 401) {
        // Token 失效，清除并跳转
        tokenService.removeToken();
        message = '登录已过期，请重新登录';
        router.navigate(['/auth/login']);
      } else if (error.status === 403) {
        message = '没有权限执行此操作';
      } else if (error.error?.message) {
        message = error.error.message;
      } else if (error.status >= 500) {
        message = '服务器内部错误';
      }

      // 登录请求的错误不弹 snackbar（由组件自行处理）
      if (!req.url.includes('/auth/login')) {
        snackBar.open(message, '关闭', { duration: 4000, panelClass: ['error-snackbar'] });
      }

      return throwError(() => error);
    }),
  );
};
