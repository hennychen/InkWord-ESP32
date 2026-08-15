import { Injectable } from '@angular/core';

/** Token 存储 key */
const TOKEN_KEY = 'inkword_token';
const USERNAME_KEY = 'inkword_username';

/**
 * JWT Token 持久化服务 (A-02)。
 * 封装 localStorage 读写，供 AuthService 与 Interceptor 使用。
 */
@Injectable({ providedIn: 'root' })
export class TokenService {
  getToken(): string | null {
    return localStorage.getItem(TOKEN_KEY);
  }

  setToken(token: string): void {
    localStorage.setItem(TOKEN_KEY, token);
  }

  removeToken(): void {
    localStorage.removeItem(TOKEN_KEY);
  }

  getUsername(): string | null {
    return localStorage.getItem(USERNAME_KEY);
  }

  setUsername(username: string): void {
    localStorage.setItem(USERNAME_KEY, username);
  }

  /** Token 是否存在（不做过期校验，过期由后端 401 触发跳转） */
  isAuthenticated(): boolean {
    return !!this.getToken();
  }
}
