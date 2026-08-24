using Hangfire;
using InkWord.Infrastructure.DbContext;
using Microsoft.EntityFrameworkCore;
using Microsoft.Extensions.Logging;

namespace InkWord.Jobs;

/// <summary>
/// Hangfire 定时任务集合（B-19/B-20/B-21）。
/// 使用 RecurringJob.AddOrUpdate&lt;T&gt; 静态泛型方法，Hangfire 自动从 DI 解析 T。
/// 需在 Program.cs 中先注册 JobStorage 与任务类（AddTransient）。
/// </summary>
public static class JobRegistrar
{
    public static void Register()
    {
        // B-20 每天 08:00 计算待复习词
        RecurringJob.AddOrUpdate<DailyPushJob>(
            "daily-review-push",
            j => j.RunAsync(),
            Cron.Daily(8),
            TimeZoneInfo.Local);

        // B-21 每周日 00:00 冷词归档
        RecurringJob.AddOrUpdate<CleanupJob>(
            "weekly-cold-word-archive",
            j => j.RunAsync(),
            Cron.Weekly(DayOfWeek.Sunday, 0),
            TimeZoneInfo.Local);

        // M1 路径 B（2026-08-22）：每天 02:00 AI 词库内容批量生成。
        // 夜间窗口只跑分级例句（kind=0，直接下发设备）；词根/辨析走
        // 管理端手动触发。限流/重试/熔断见 AiContentJob。
        RecurringJob.AddOrUpdate<AiContentJob>(
            "nightly-ai-content",
            j => j.RunAsync(0, null, 500, CancellationToken.None),
            Cron.Daily(2),
            TimeZoneInfo.Local);

        // P0B（2026-08-24）：每天 03:00 词条 TTS 批量合成（错开 AI 内容任务）。
        // 补齐 data/audio/{Id:N}.mp3 缺失词条；幂等，引擎不可用时止损空转。
        RecurringJob.AddOrUpdate<TtsJob>(
            "nightly-tts",
            j => j.RunAsync(2000, CancellationToken.None),
            Cron.Daily(3),
            TimeZoneInfo.Local);

        // P2A（2026-08-24）：每小时回收过期对话音频（chat_*.mp3 超 1 小时）。
        RecurringJob.AddOrUpdate<ChatAudioCleanupJob>(
            "hourly-chat-audio-cleanup",
            j => j.RunAsync(),
            Cron.Hourly(),
            TimeZoneInfo.Local);
    }
}

/// <summary>每日复习提醒（B-20）：统计每个设备的到期词数。</summary>
public class DailyPushJob
{
    private readonly AppDbContext _db;
    private readonly ILogger<DailyPushJob> _logger;

    public DailyPushJob(AppDbContext db, ILogger<DailyPushJob> logger)
    {
        _db = db;
        _logger = logger;
    }

    public async Task RunAsync()
    {
        var now = DateTime.UtcNow;
        var due = await _db.LearningRecords.AsNoTracking()
            .Where(r => r.NextReview <= now)
            .GroupBy(r => r.DeviceId)
            .Select(g => new { DeviceId = g.Key, Count = g.Count() })
            .ToListAsync();

        foreach (var d in due)
            _logger.LogInformation("设备 {DeviceId} 有 {Count} 个待复习词", d.DeviceId, d.Count);

        _logger.LogInformation("DailyPushJob 完成，涉及 {N} 台设备", due.Count);
        // TODO: 推送至消息队列 / WebSocket 通知设备
    }
}

/// <summary>冷词归档（B-21）：2 年内无人学习的词标记 Archived。</summary>
public class CleanupJob
{
    private readonly AppDbContext _db;
    private readonly ILogger<CleanupJob> _logger;

    public CleanupJob(AppDbContext db, ILogger<CleanupJob> logger)
    {
        _db = db;
        _logger = logger;
    }

    public async Task RunAsync()
    {
        var threshold = DateTime.UtcNow.AddYears(-2);
        var learnedWordIds = _db.LearningRecords.AsNoTracking()
            .Where(r => r.LastStudiedAt >= threshold)
            .Select(r => r.WordId).Distinct();

        var cold = await _db.Words.Where(w => !w.Archived && !learnedWordIds.Contains(w.Id))
            .ToListAsync();
        foreach (var w in cold) w.Archived = true;

        var saved = await _db.SaveChangesAsync();
        _logger.LogInformation("冷词归档完成：标记 {Count} 条", saved);
    }
}
