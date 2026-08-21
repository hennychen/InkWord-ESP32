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
