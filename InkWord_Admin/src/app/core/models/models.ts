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
  username: string;
  token: string;
  /** 有效期（秒） */
  expiresIn: number;
}

// ── Word ──────────────────────────────────────────────
/** 与后端 Word 实体 JSON（camelCase）严格对齐：definition/tags[]/audioFile
 *  为历史错位命名，2026-08-20 修正（词库扩展四字段一并落地） */
export interface Word {
  id: string;
  text: string;
  phonetic?: string;
  meaning: string;
  example?: string;
  audio?: string;
  tag?: string;
  difficulty: number;
  /** 词库扩展四字段（V2.1 §6.2，2026-08-20） */
  root?: string;         // 词根词缀 “spect=看; vis=看”
  inflections?: string;  // 派生变形（逗号分隔）
  source?: string;       // 教材来源 “人教版 九年级 Unit 5”
  grade?: string;        // 年级 “九年级”
  version: number;
  changeType: number;
  archived: boolean;
  createdAt?: string;
  /** AI 内容状态（M1）：0=无 1=待审 2=已应用 3=生成失败 */
  aiStatus?: number;
}

export interface WordCreateDto {
  text: string;
  phonetic?: string;
  meaning: string;
  example?: string;
  audio?: string;
  tag?: string;
  difficulty: number;
  root?: string;
  inflections?: string;
  source?: string;
  grade?: string;
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

// ── 今日学习统计（v1.3 T3.2）─────────────────────
/** 今日学习统计（后端 TodayStatsResp，camelCase）。
 * 口径近似：今日 = lastStudiedAt ≥ 当日 0 点；首学/复习以 reviewCount 1/>1 近似；
 * 答对/答错以最后评分 lastQuality ≥/<3 近似（与固件 quality 口径一致）。 */
export interface TodayStats {
  activeDevices: number;
  touchedRecords: number;
  newWords: number;
  reviewWords: number;
  correctToday: number;
  wrongToday: number;
  avgQuality: number;
}

// ── WrongBook (P1) ──────────────────────────────────
/** 错词排行条目（后端 WrongTopItem，camelCase） */
export interface WrongTopItem {
  wordText: string;
  meaning: string;
  wrongCount: number;
  learners: number;
}

export interface WrongTopResp {
  items: WrongTopItem[];
  collectedRecords: number;
  masteredRecords: number;
}

// ── AI 内容增强（M1 路径 B，2026-08-22）─────────────────
/** AI 批量生成触发：kind 0=分级例句 1=词根助记 2=易混辨析；
 * subject 科目占位符（v1.3 T3.3，空=英语默认；v1.5 全科生成入口） */
export interface AiGenerateReq {
  kind: number;
  tag?: string;
  subject?: string;
  limit?: number;
}

/** 待审条目：现值 vs AI 建议（AiSuggestion 服务端解析后下发）；
 * suggestedFront/Back/Phonetic/Meaning 为 T5.4 卡组条目建议（kind=3 专用） */
export interface AiPendingItem {
  id: string;
  text: string;
  meaning: string;
  tag: string;
  grade: string;
  currentExample: string;
  currentRoot: string;
  suggestedExample?: string | null;
  suggestedRoot?: string | null;
  suggestedConfusionNote?: string | null;
  kind: number;
  createdAt: string;
  suggestedFront?: string | null;
  suggestedBack?: string | null;
  suggestedPhonetic?: string | null;
  suggestedMeaning?: string | null;
}

/** 审核通过（可携带编辑终值；null = 采用建议原值）；
 * front/back/phonetic/meaning 为 T5.4 kind=3 卡组条目终值 */
export interface AiApplyReq {
  example?: string | null;
  root?: string | null;
  front?: string | null;
  back?: string | null;
  phonetic?: string | null;
  meaning?: string | null;
}

/** T5.4 AI 卡组生成触发：素材（课文/知识点清单）+ 条数上限 */
export interface DeckGenReq {
  source?: string;
  limit?: number;
}

/** 管理端卡组概览（T5.4 AI 生成弹窗下拉） */
export interface AdminDeckInfo {
  id: string;
  code: string;
  name: string;
  payloadType: string;
  subjectId?: string | null;
  itemCount: number;
}

// ── Book（阅读器后端，2026-09）─────────────────────
/** 书籍列表行（AdminBookController List 匿名投影，camelCase） */
export interface BookListItem {
  id: string;
  bookKey: string;
  title: string;
  author?: string;
  language: string;
  tags?: string;
  /** 字节数 */
  fileSize: number;
  format: string;
  published: boolean;
  downloadCount: number;
  createdAt?: string;
}

/** 书籍详情（含阅读统计 readerCount / avgProgressPct） */
export interface BookDetail extends BookListItem {
  description?: string;
  readerCount: number;
  avgProgressPct: number;
}

/** 上传元数据（multipart 表单字段，同后端 BookCreateDto） */
export interface BookCreateDto {
  title: string;
  author?: string;
  language: string;
  tags?: string;
  description?: string;
}

/** 元数据更新（同后端 BookUpdateDto：null = 不修改） */
export interface BookUpdateDto {
  title?: string;
  author?: string;
  language?: string;
  tags?: string;
  description?: string;
  published?: boolean;
}

/** 列表查询参数 */
export interface BookQuery {
  page: number;
  size: number;
  keyword?: string;
  language?: string;
  published?: boolean;
}

// ── 阅读统计（AdminDashboardController，2026-09）──────────
export interface ReadingStatsResp {
  totalBooks: number;
  activeReadersToday: number;
  totalReadMinutesToday: number;
  popularBooks: PopularBookItem[];
  dailyReading: DailyReadingItem[];
}

export interface PopularBookItem {
  bookKey: string;
  title: string;
  readers: number;
  avgProgressPct: number;
}

export interface DailyReadingItem {
  date: string;
  minutes: number;
  readers: number;
}

/** 单设备阅读记录行（device-reading 端点） */
export interface DeviceBookReadingItem {
  bookKey: string;
  title: string;
  currentPage: number;
  totalPages: number;
  progressPct: number;
  lastReadAt: string;
  totalReadMinutes: number;
}

export interface DeviceReadingResp {
  books: DeviceBookReadingItem[];
}

// ── Deck/Subject（P3 卡组管理，2026-09）─────────────
/** 科目行（AdminSubjectController：含每科目卡组计数） */
export interface AdminSubjectItem {
  id: string;
  code: string;
  name: string;
  sortOrder: number;
  deckCount: number;
}

/** 卡组列表行（AdminDeckController List 增强：含科目/Owner/条目计数/最高版本） */
export interface DeckListItem {
  id: string;
  code: string;
  name: string;
  payloadType: string;
  subjectId?: string | null;
  subjectCode: string;
  subjectName: string;
  itemCount: number;
  /** 条目最高 Version（未归档口径，设备增量同步游标参照） */
  maxVersion: number;
  isShared: boolean;
  ownerName: string;
  updatedAt?: string;
}

/** 卡组详情（含学习覆盖：learnerCount 学过设备数 / studiedItems 被学条目数） */
export interface DeckDetail {
  id: string;
  code: string;
  name: string;
  payloadType: string;
  description?: string | null;
  isShared: boolean;
  sharedAt?: string | null;
  createdAt?: string;
  subjectCode: string;
  subjectName: string;
  ownerId?: string | null;
  ownerName?: string | null;
  itemCount: number;
  maxVersion: number;
  learnerCount: number;
  studiedItems: number;
}

/** 卡组条目行（Items 分页；含归档行删除痕迹） */
export interface DeckItem {
  id: string;
  version: number;
  text: string;
  front: string;
  back: string;
  phonetic?: string | null;
  meaning?: string | null;
  example?: string | null;
  archived: boolean;
}

/** 条目分页响应（{ items, total, page, size } 信封内层） */
export interface DeckItemsResp {
  items: DeckItem[];
  total: number;
  page: number;
  size: number;
}

/** 条目写请求（同后端 AdminItemReq：FillWord 同源映射，
 * Front/Back 双写 Text/Meaning，通用列 front/back/phonetic/example） */
export interface DeckItemReq {
  front: string;
  back: string;
  phonetic?: string;
  example?: string;
}

// ── SRS 算法对比（M3 路径 A）─────────────────────
export interface SrsComparisonItem {
  algorithm: string;
  dueToday: number;
  dueWeek: number;
  dueMonth: number;
  future: number;
  avgIntervalDays: number;
}

export interface SrsComparisonResp {
  sm2Records: number;
  fsrsRecords: number;
  items: SrsComparisonItem[];
}
