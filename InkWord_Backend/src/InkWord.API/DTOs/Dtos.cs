namespace InkWord.API.DTOs;

// ====== 设备端 DTO ======

public record RegisterReq(string Mac, string? Name);
public record RegisterResp(Guid DeviceId, string ApiKey);

public record SyncReq(int LocalVersion, int Count);

/// <summary>设备词库条目（旧 11 字段为主集；v2 全科地基字段为可选参数，
/// 调用方填实值 —— 旧固件 cJSON 按名取值，未知/多余键天然忽略，
/// 兼容性由 native 用例固化）。
/// ChangeType：0=新增 1=更新 2=删除墓碑（协议 v3 2026-09-08——墓碑仅携
/// text/tag/version 身份键 + deck 归属，内容字段空，设备按 (text,tag) 删除）</summary>
public record WordDto(string Text, string Phonetic, string Meaning, string Example,
                      string Audio, string Tag, int Difficulty, int Version, int ChangeType,
                      string? Subject = null, string? DeckId = null, string? PayloadType = null,
                      string? Front = null, string? Back = null, string? PayloadJson = null);
public record SyncResp(int NewVersion, List<WordDto> Words);

public record ProgressItem(Guid WordId, int Quality, long Timestamp);

/// <summary>收藏上报（设备端 SET 长按切换后同步）</summary>
public record CollectReq(Guid WordId, bool Collected);

/// <summary>墨封上报（设备端 toggle_master 切换后同步，2026-09-04）</summary>
public record MasterReq(Guid WordId, bool Mastered);

public record HeartbeatReq(int Battery, string Version);

public record OtaCheckReq(string CurrentVer);
public record OtaCheckResp(bool HasUpdate, string Url, string Md5, int Size, string Version);

// ====== 管理端 DTO ======

public record WordCreateDto(string Text, string Phonetic, string Meaning,
                            string Example, string? Audio = null, string Tag = "",
                            int Difficulty = 3,
                            string? Root = null, string? Inflections = null,
                            string? Source = null, string? Grade = null);

public record WordUpdateDto(Guid Id, string Text, string Phonetic, string Meaning,
                            string Example, string? Audio = null, string Tag = "",
                            int Difficulty = 3,
                            string? Root = null, string? Inflections = null,
                            string? Source = null, string? Grade = null);

public record WordQueryDto(int Page = 1, int Size = 20, string? Tag = null,
                           int? Difficulty = null, string? Keyword = null);

public record OtaPkgUploadDto(string Version, string ReleaseNotes, bool Published);

public record CommandReq(string Action);   // "force_sync" / "force_refresh" / "push_ota"

// ====== 看板 ======

public record DashboardStats(int TotalWords, int TotalDevices, int OnlineDevices,
                             int ActiveLearnersToday, double AvgStudyMinutes,
                             List<SrsDistributionItem> SrsDistribution,
                             List<DailyActiveItem> DailyActive);

public record SrsDistributionItem(int Level, int Count);
public record DailyActiveItem(DateTime Date, int Count);

// ====== 错词本（学习分析） ======

public record WrongTopItem(string WordText, string Meaning, int WrongCount, int Learners);
public record WrongTopResp(List<WrongTopItem> Items, int CollectedRecords, int MasteredRecords);

// ====== 今日学习统计（v1.3 T3.2，2026-08-24） ======

/// <summary>今日学习统计（LearningRecord 按日聚合，无需新设备协议）。
/// 口径近似：LearningRecord 是 (Device,Word) 状态记录而非事件流，
/// 今日 = LastStudiedAt ≥ 当日 0 点；首学/复习以 ReviewCount 1/》1 近似；
/// 答对/答错以最后评分 LastQuality ≥/＜3 近似（与固件 quality 口径一致）。</summary>
public record TodayStatsResp(int ActiveDevices, int TouchedRecords,
    int NewWords, int ReviewWords, int CorrectToday, int WrongToday,
    double AvgQuality);

// ====== 分页结果 ======

public record PagedResult<T>(IReadOnlyList<T> Items, int Total, int Page, int Size);

// ====== AI 内容增强（M1 路径 B，2026-08-22） ======

/// <summary>手动触发 AI 批量生成：kind 0=分级例句 1=词根助记 2=易混辨析；limit 0=默认上限；
/// subject 科目占位符（v1.3 T3.3 参数化，null/空=英语默认；v1.5 全科生成入口）</summary>
public record AiGenerateReq(int Kind = 0, string? Tag = null, string? Subject = null, int Limit = 0);

/// <summary>待审条目：现值 vs AI 建议（AiSuggestion JSON 服务端解析后下发）；
/// SuggestedFront/Back/Phonetic/Meaning 为 T5.4 卡组条目建议（kind=3 专用）</summary>
public record AiPendingItem(Guid Id, string Text, string Meaning, string Tag, string Grade,
    string CurrentExample, string CurrentRoot,
    string? SuggestedExample, string? SuggestedRoot, string? SuggestedConfusionNote,
    int Kind, DateTime CreatedAt,
    string? SuggestedFront = null, string? SuggestedBack = null,
    string? SuggestedPhonetic = null, string? SuggestedMeaning = null);

/// <summary>审核通过（可携带编辑后的终值；null = 采用建议原值）；
/// Front/Back/Phonetic/Meaning 为 T5.4 kind=3 卡组条目终值</summary>
public record AiApplyReq(string? Example, string? Root,
    string? Front = null, string? Back = null,
    string? Phonetic = null, string? Meaning = null);

/// <summary>T5.4 AI 卡组生成触发：素材（课文/知识点清单）+ 条数上限</summary>
public record DeckGenReq(string? Source = null, int Limit = 10);

// ====== SRS 算法对比（M3 路径 A） ======

public record SrsComparisonItem(string Algorithm, int DueToday, int DueWeek, int DueMonth,
    int Future, double AvgIntervalDays);
public record SrsComparisonResp(int Sm2Records, int FsrsRecords, List<SrsComparisonItem> Items);

// ====== 阅读器设备端 DTO（2026-09-05） ======

/// <summary>阅读进度上报（设备端退出阅读/翻页时推送）</summary>
public record ReadingProgressReq(
    string BookKey,       // 书籍标识（文件名去扩展名）
    uint Signature,       // 内容签名
    int CurrentPage,
    int TotalPages,
    int FontLevel,
    int ReadMinutes       // 本次阅读时长（分钟）
);

/// <summary>书签批量同步请求（设备端退出书签管理时推送全量）</summary>
public record BookmarkSyncReq(
    string BookKey,
    uint Signature,
    List<BookmarkItem> Bookmarks
);
public record BookmarkItem(int Page, uint ByteOffset, string Note);

/// <summary>云端书籍列表项（设备端拉取）</summary>
public record BookDto(
    string BookKey,
    string Title,
    string Author,
    string Language,
    string Tags,
    long FileSize,
    string Format,
    string Description,
    int DownloadCount
);

// ====== 阅读器管理端 DTO（2026-09-05） ======

/// <summary>书籍创建/编辑</summary>
public record BookCreateDto(
    string Title,
    string? Author = null,
    string Language = "zh",
    string? Tags = null,
    string? Description = null
);

public record BookUpdateDto(
    string? Title = null,
    string? Author = null,
    string? Language = null,
    string? Tags = null,
    string? Description = null,
    bool? Published = null
);

public record BookQueryDto(int Page = 1, int Size = 20, string? Keyword = null,
                           string? Language = null, bool? Published = null);

/// <summary>阅读统计（管理看板扩展）</summary>
public record ReadingStatsResp(
    int TotalBooks,
    int ActiveReadersToday,
    int TotalReadMinutesToday,
    List<PopularBookItem> PopularBooks,
    List<DailyReadingItem> DailyReading
);

public record PopularBookItem(string BookKey, string Title, int Readers, int AvgProgressPct);
public record DailyReadingItem(string Date, int Minutes, int Readers);

/// <summary>设备阅读详情（管理端查看单设备阅读情况）</summary>
public record DeviceReadingDetailResp(
    List<DeviceBookReadingItem> Books
);
public record DeviceBookReadingItem(
    string BookKey, string Title, int CurrentPage, int TotalPages,
    int ProgressPct, DateTime LastReadAt, int TotalReadMinutes
);
