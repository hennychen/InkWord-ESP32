import { Injectable, inject, signal } from '@angular/core';
import { HttpClient } from '@angular/common/http';
import { Router } from '@angular/router';
import { Observable, tap } from 'rxjs';
import { environment } from '../../../environments/environment';
import { LoginRequest, LoginResponse, ApiResponse } from '../models/models';
import { TokenService } from './token.service';

/**
 * 认证服务 (A-03)：处理登录、登出逻辑。
 * 登录成功后将 JWT Token 持久化至 localStorage。
 */
@Injectable({ providedIn: 'root' })
export class AuthService {
  private readonly http = inject(HttpClient);
  private readonly router = inject(Router);
  private readonly tokenService = inject(TokenService);

  /** 当前用户名（Signal，供 UI 响应式显示） */
  readonly username = signal<string | null>(this.tokenService.getUsername());

  /** 登录 */
  login(credentials: LoginRequest): Observable<ApiResponse<LoginResponse>> {
    return this.http
      .post<ApiResponse<LoginResponse>>(`${environment.apiUrl}/auth/login`, credentials)
      .pipe(
        tap((res) => {
          if (res.code === 0 && res.data?.token) {
            this.tokenService.setToken(res.data.token);
            this.tokenService.setUsername(res.data.username);
            this.username.set(res.data.username);
          }
        }),
      );
  }

  /** 登出 */
  logout(): void {
    this.tokenService.removeToken();
    this.username.set(null);
    this.router.navigate(['/auth/login']);
  }

  /** 是否已认证 */
  isAuthenticated(): boolean {
    return this.tokenService.isAuthenticated();
  }
}
