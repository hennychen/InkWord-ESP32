namespace InkWord.API.DTOs;

// ====== 设备端 DTO ======

public record RegisterReq(string Mac, string? Name);
public record RegisterResp(Guid DeviceId, string ApiKey);

public record SyncReq(int LocalVersion, int Count);
public record WordDto(string Text, string Phonetic, string Meaning, string Example,
                      string Audio, string Tag, int Difficulty, int Version, int ChangeType);
public record SyncResp(int NewVersion, List<WordDto> Words);

public record ProgressItem(Guid WordId, int Quality, long Timestamp);

/// <summary>收藏上报（设备端 SET 长按切换后同步）</summary>
public record CollectReq(Guid WordId, bool Collected);

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
public record WrongTopResp(List<WrongTopItem> Items, int CollectedRecords);

// ====== 分页结果 ======

public record PagedResult<T>(IReadOnlyList<T> Items, int Total, int Page, int Size);

// ====== AI 内容增强（M1 路径 B，2026-08-22） ======

/// <summary>手动触发 AI 批量生成：kind 0=分级例句 1=词根助记 2=易混辨析；limit 0=默认上限</summary>
public record AiGenerateReq(int Kind = 0, string? Tag = null, int Limit = 0);

/// <summary>待审条目：现值 vs AI 建议（AiSuggestion JSON 服务端解析后下发）</summary>
public record AiPendingItem(Guid Id, string Text, string Meaning, string Tag, string Grade,
    string CurrentExample, string CurrentRoot,
    string? SuggestedExample, string? SuggestedRoot, string? SuggestedConfusionNote,
    int Kind, DateTime CreatedAt);

/// <summary>审核通过（可携带编辑后的终值；null = 采用建议原值）</summary>
public record AiApplyReq(string? Example, string? Root);

// ====== SRS 算法对比（M3 路径 A） ======

public record SrsComparisonItem(string Algorithm, int DueToday, int DueWeek, int DueMonth,
    int Future, double AvgIntervalDays);
public record SrsComparisonResp(int Sm2Records, int FsrsRecords, List<SrsComparisonItem> Items);
