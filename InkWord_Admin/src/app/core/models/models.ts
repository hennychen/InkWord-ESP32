/**
 * 前端数据模型 — 与后端 DTO 保持字段一致（camelCase）。
 * ============================================================ */

/** 统一 API 响应包装 */
export interface ApiResponse<T> {
  code: number;
  message: string;
  data: T;
}

/** 分页结果 */
export interface PagedResult<T> {
  items: T[];
  total: number;
  page: number;
  size: number;
}

// ── Auth ──────────────────────────────────────────────
export interface LoginRequest {
  username: string;
  password: string;
}

export interface LoginResponse {
  token: string;
  expiresAt: string;
  username: string;
}

// ── Word ──────────────────────────────────────────────
export interface Word {
  id: string;
  text: string;
  phonetic?: string;
  definition: string;
  example?: string;
  audioFile?: string;
  tags: string[];
  difficulty: number;
  source?: string;
  archived: boolean;
  createdAt: string;
}

export interface WordCreateDto {
  text: string;
  phonetic?: string;
  definition: string;
  example?: string;
  audioFile?: string;
  tags: string[];
  difficulty: number;
  source?: string;
}

export interface WordUpdateDto extends WordCreateDto {
  id: string;
}

export interface WordQueryDto {
  page: number;
  size: number;
  keyword?: string;
  tag?: string;
  difficulty?: number;
}

// ── Device ────────────────────────────────────────────
export interface Device {
  id: string;
  deviceCode: string;
  name?: string;
  macAddress: string;
  firmwareVersion: string;
  batteryLevel: number;
  lastHeartbeat?: string;
  online: boolean;
  createdAt: string;
}

export interface DeviceCommandDto {
  deviceId: string;
  action: 'force_sync' | 'force_refresh' | 'push_ota';
  payload?: Record<string, unknown>;
}

// ── OTA ───────────────────────────────────────────────
export interface OtaPackage {
  id: string;
  version: string;
  downloadUrl: string;
  md5: string;
  description?: string;
  createdAt: string;
}

// ── Dashboard ─────────────────────────────────────────
export interface DashboardStats {
  totalWords: number;
  totalDevices: number;
  activeDevicesToday: number;
  learningUsersToday: number;
  avgStudyMinutes: number;
}

export interface SrsDistribution {
  level: string;
  count: number;
}

export interface DailyActiveData {
  date: string;
  count: number;
}
