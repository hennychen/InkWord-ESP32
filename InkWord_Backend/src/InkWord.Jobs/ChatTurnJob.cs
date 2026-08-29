using Hangfire;
using InkWord.Core.Entities;
using InkWord.Infrastructure.Cache;
using InkWord.Infrastructure.DbContext;
using InkWord.Services;
using Microsoft.EntityFrameworkCore;
using Microsoft.Extensions.AI;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;

namespace InkWord.Jobs;

/// <summary>
/// 对话轮落库执行体（A3，2026-08-29）：ChatService 经 HangfireChatTurnSink
/// Enqueue 的 fire-and-forget 目标——对话端点响应不等待，本类在 Hangfire
/// worker 里完成实际写库。失败仅日志（AutomaticRetry(0)：对话日志丢失
/// 可接受，重试风暴不可接受，对齐 TtsJob 止损先例）。
/// </summary>
public class ChatTurnLogger
{
    private readonly AppDbContext _db;
    private readonly ILogger<ChatTurnLogger> _logger;

    public ChatTurnLogger(AppDbContext db, ILogger<ChatTurnLogger> logger)
    {
        _db = db;
        _logger = logger;
    }

    [AutomaticRetry(Attempts = 0)]
    public async Task ExecuteAsync(ChatTurn turn)
    {
        try
        {
            _db.ChatTurns.Add(turn);
            await _db.SaveChangesAsync();
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex, "ChatTurn 落库失败（丢弃，不影响对话）：{Device}",
                turn.DeviceId);
        }
    }
}

/// <summary>
/// ChatService 落库出口的 Hangfire 实现（A3）：Enqueue 即返回，
/// 执行体见 ChatTurnLogger。无状态可 Singleton（BackgroundJob 静态客户端）。
/// </summary>
public class HangfireChatTurnSink : IChatTurnSink
{
    public void Enqueue(ChatTurn turn) =>
        BackgroundJob.Enqueue<ChatTurnLogger>(l => l.ExecuteAsync(turn));
}

/// <summary>
/// 对话轮保留策略（A3）：每天 04:00 清理 90 天前记录（对齐心跳类数据
/// 保留粒度；周报生成后原始轮次价值衰减，90 天足够回看与纠错）。
/// </summary>
public class ChatTurnCleanupJob
{
    private readonly AppDbContext _db;
    private readonly ILogger<ChatTurnCleanupJob> _logger;

    public ChatTurnCleanupJob(AppDbContext db, ILogger<ChatTurnCleanupJob> logger)
    {
        _db = db;
        _logger = logger;
    }

    [AutomaticRetry(Attempts = 0)]
    public async Task RunAsync()
    {
        var cutoff = DateTime.UtcNow.AddDays(-90);
        var removed = await _db.ChatTurns
            .Where(t => t.Ts < cutoff)
            .ExecuteDeleteAsync();
        if (removed > 0)
            _logger.LogInformation("ChatTurnCleanup：清理 {Count} 条 90 天前对话轮", removed);
    }
}

/// <summary>
/// 词库快照提供者（A3 wordHits 扫描源）：全库未归档词（L1 命中本身
/// 稀疏，设备词书维度一期不分），5 分钟时间片缓存节流——对话每轮
/// 查询不打库；缓存窗口内新入库词不可见（教学词库日更粒度，可接受）。
/// 竞态最坏双读，幂等无风险。Singleton 注册：经 IServiceScopeFactory
/// 取短生命周期 DbContext（避免 captive dependency）。
/// </summary>
public class CachedWordListProvider : IWordListProvider
{
    private static readonly TimeSpan RefreshInterval = TimeSpan.FromMinutes(5);

    private readonly IServiceScopeFactory _scopeFactory;
    private readonly ILogger<CachedWordListProvider> _logger;
    private volatile IReadOnlyList<VocabEntry>? _cache;
    private long _loadedAtTicks; // volatile 不支持 DateTime：以 Ticks 承载，竞态最坏双读（见类注释）

    public CachedWordListProvider(IServiceScopeFactory scopeFactory,
        ILogger<CachedWordListProvider> logger)
    {
        _scopeFactory = scopeFactory;
        _logger = logger;
    }

    public async ValueTask<IReadOnlyList<VocabEntry>> GetWordsAsync(CancellationToken ct)
    {
        var cache = _cache;
        if (cache is not null && DateTime.UtcNow.Ticks - Volatile.Read(ref _loadedAtTicks) < RefreshInterval.Ticks)
            return cache;

        using var scope = _scopeFactory.CreateScope();
        var db = scope.ServiceProvider.GetRequiredService<AppDbContext>();
        cache = await db.Words.AsNoTracking()
            .Where(w => !w.Archived)
            .Select(w => new VocabEntry(w.Text, w.Id.ToString()))
            .ToListAsync(ct);
        _cache = cache;
        Volatile.Write(ref _loadedAtTicks, DateTime.UtcNow.Ticks);
        _logger.LogDebug("词库快照刷新：{Count} 词", cache.Count);
        return cache;
    }
}

/// <summary>
/// AI 对话周报（A3，2026-08-29）：每周日 05:00 聚合各设备近 7 天
/// ChatTurn → LLM 结构化复盘（summary/topics/highlights/suggestion/
/// reviewWords 五段 JSON）→ ChatReviews 表留痕 + Redis
/// chatreview:{deviceId} TTL 7 天（设备端读零延迟）。
///
/// 输入防注入：每轮文本截 200 字符、抽样 ≤60 轮（周报是摘要非全量）；
/// LLM 失败跳过该设备本周（下周重跑天然幂等：同周唯一索引 upsert）。
/// </summary>
public class ChatReviewJob
{
    /// <summary>聚合窗口</summary>
    private static readonly TimeSpan Window = TimeSpan.FromDays(7);

    /// <summary>单设备送入 LLM 的轮次上限（超出取最近）</summary>
    private const int MaxTurnsPerDevice = 60;

    private const int TurnTextMaxChars = 200;

    private readonly AppDbContext _db;
    private readonly IChatClient _chat;
    private readonly IRedisCache _cache;
    private readonly ILogger<ChatReviewJob> _logger;

    public ChatReviewJob(AppDbContext db, IChatClient chat, IRedisCache cache,
        ILogger<ChatReviewJob> logger)
    {
        _db = db;
        _chat = chat;
        _cache = cache;
        _logger = logger;
    }

    [AutomaticRetry(Attempts = 0)]
    public async Task RunAsync(CancellationToken ct)
    {
        var since = DateTime.UtcNow - Window;

        // 近 7 天有对话的设备（轮数倒序，LLM 繁忙时优先高活设备）
        var devices = await _db.ChatTurns.AsNoTracking()
            .Where(t => t.Ts >= since)
            .GroupBy(t => t.DeviceId)
            .Select(g => new { DeviceId = g.Key, Count = g.Count() })
            .OrderByDescending(d => d.Count)
            .ToListAsync(ct);
        if (devices.Count == 0)
        {
            _logger.LogInformation("ChatReviewJob：近 7 天无对话，跳过");
            return;
        }

        var done = 0;
        foreach (var d in devices)
        {
            if (ct.IsCancellationRequested) break;
            if (await ReviewDeviceAsync(d.DeviceId, since, ct)) done++;
        }
        _logger.LogInformation("ChatReviewJob 完成：{Done}/{Total} 台设备", done, devices.Count);
    }

    private async Task<bool> ReviewDeviceAsync(Guid deviceId, DateTime since, CancellationToken ct)
    {
        try
        {
            var turns = await _db.ChatTurns.AsNoTracking()
                .Where(t => t.DeviceId == deviceId && t.Ts >= since)
                .OrderByDescending(t => t.Ts)
                .Take(MaxTurnsPerDevice)
                .ToListAsync(ct);
            if (turns.Count == 0) return false;

            var weekStart = turns.Max(t => t.Ts).Date;
            while (weekStart.DayOfWeek != DayOfWeek.Monday) weekStart = weekStart.AddDays(-1);

            var payload = await GenerateReviewAsync(turns, ct);
            if (string.IsNullOrWhiteSpace(payload))
            {
                _logger.LogWarning("ChatReviewJob：设备 {Device} LLM 输出为空，跳过本周", deviceId);
                return false;
            }

            // 表留痕（同周唯一 upsert）+ Redis 热路径（TTL 7 天到下周生成点）
            var existing = await _db.ChatReviews
                .FirstOrDefaultAsync(r => r.DeviceId == deviceId && r.WeekStart == weekStart, ct);
            if (existing is null)
            {
                _db.ChatReviews.Add(new ChatReview
                {
                    DeviceId = deviceId,
                    WeekStart = weekStart,
                    PayloadJson = payload,
                    TurnCount = turns.Count,
                });
            }
            else
            {
                existing.PayloadJson = payload;
                existing.TurnCount = turns.Count;
            }
            await _db.SaveChangesAsync(ct);

            await TrySaveCacheAsync(deviceId, weekStart, turns.Count, payload, ct);
            return true;
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            _logger.LogWarning(ex, "ChatReviewJob：设备 {Device} 复盘失败（跳过，下周重跑）", deviceId);
            return false;
        }
    }

    private async Task<string> GenerateReviewAsync(List<ChatTurn> turns, CancellationToken ct)
    {
        var log = string.Join("\n", turns
            .OrderBy(t => t.Ts)
            .Select(t => $"- student: {Truncate(t.Transcript)}\n  tutor: {Truncate(t.Reply)}"));

        var response = await _chat.GetResponseAsync(
            $$"""
            You are reviewing a young Chinese student's English conversation logs from a vocabulary device.
            Below are the conversation turns from the last 7 days (student / tutor).
            Write a weekly review in JSON with EXACTLY these keys:
            - "summary": one warm encouraging sentence in Chinese (<= 40 chars)
            - "topics": 2-3 topic keywords in Chinese (array of strings)
            - "highlights": up to 3 good English expressions the student used (array, from the logs)
            - "suggestion": one practice scenario to try next week, in Chinese (<= 30 chars)
            - "reviewWords": up to 5 English words worth reviewing (array, from the logs)
            Reply with ONLY the JSON object. No markdown fences, no extra text.

            Conversation logs:
            {{log}}
            """,
            new ChatOptions { Temperature = 0.5f }, ct);

        return StripFences(response.Text.Trim());
    }

    /// <summary>LLM 输出清洗：剥 markdown 围栏（qwen 偶发），仅保留首行 { 到尾行 } 的 JSON 主体</summary>
    private static string StripFences(string text)
    {
        if (text.StartsWith("```"))
        {
            var lines = text.Split('\n');
            var json = string.Join('\n', lines.Skip(1).TakeWhile(l => !l.TrimStart().StartsWith("```")));
            return json.Trim();
        }
        return text;
    }

    private static string Truncate(string s) =>
        s.Length <= TurnTextMaxChars ? s : s[..TurnTextMaxChars];

    private async Task TrySaveCacheAsync(Guid deviceId, DateTime weekStart,
        int turnCount, string payload, CancellationToken ct)
    {
        try
        {
            await _cache.SetAsync($"chatreview:{deviceId:N}",
                new ChatReviewCache(weekStart, turnCount, payload),
                Window, ct);
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            _logger.LogWarning(ex, "ChatReview Redis 写入失败（表已留痕，设备端走冷路径）");
        }
    }
}

/// <summary>周报缓存载荷（Redis chatreview:{deviceId}，端点双通道热路径）</summary>
public record ChatReviewCache(DateTime WeekStart, int TurnCount, string PayloadJson);
