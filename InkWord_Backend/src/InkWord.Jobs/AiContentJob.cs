using System.Text.Json;
using Hangfire;
using InkWord.Infrastructure.DbContext;
using InkWord.Services;
using Microsoft.EntityFrameworkCore;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.Logging;

namespace InkWord.Jobs;

/// <summary>
/// AI 词库内容批量生成任务（M1.1.4 路径 B，2026-08-22）。
///
/// 扫描 AiStatus==0 的词条，调 AiContentService 生成建议（AiStatus=1 待审），
/// 失败标 3。分页 50 条/批，页内并发受 Ai:Concurrency 限流（SemaphoreSlim，
/// LLM I/O 并行、EF 写入顺序）；每批 SaveChanges 缩短事务。
///
/// M2 加固：AutomaticRetry(2)；当日累计失败 ≥50 熔断终止（服务异常风暴
/// 保护）；单轮上限 500 条（夜间 2:00 窗口的 token 成本护栏）。
/// </summary>
public class AiContentJob
{
    private const int PageSize = 50;
    private const int MaxPerRun = 500;
    private const int DailyFailBreaker = 50;

    // 熔断计数（单实例足够；Hangfire 单 server 部署）
    private static readonly object BreakerLock = new();
    private static DateTime _breakerDate;
    private static int _breakerFails;

    private static readonly JsonSerializerOptions JsonOpts = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
    };

    private readonly AppDbContext _db;
    private readonly AiContentService _ai;
    private readonly IConfiguration _cfg;
    private readonly ILogger<AiContentJob> _logger;

    public AiContentJob(AppDbContext db, AiContentService ai,
        IConfiguration cfg, ILogger<AiContentJob> logger)
    {
        _db = db;
        _ai = ai;
        _cfg = cfg;
        _logger = logger;
    }

    /// <summary>kind：0 分级例句 1 词根助记 2 易混辨析；tag 可选过滤；subject 科目占位符
    /// （v1.3 T3.3，null=英语默认）；limit 可选覆盖单轮上限</summary>
    [AutomaticRetry(Attempts = 2)]
    public async Task RunAsync(int kind, string? tag, string? subject, int limit, CancellationToken ct)
    {
        if (!Enum.IsDefined(typeof(AiContentKind), kind))
        {
            _logger.LogWarning("AiContentJob 非法 kind={Kind}", kind);
            return;
        }
        var contentKind = (AiContentKind)kind;
        limit = limit <= 0 || limit > MaxPerRun ? MaxPerRun : limit;

        var concurrency = _cfg.GetValue("Ai:Concurrency", 2);
        using var semaphore = new SemaphoreSlim(concurrency);
        var processed = 0;
        var failed = 0;

        while (processed < limit)
        {
            if (ShouldBreak()) break;

            var pageSize = Math.Min(PageSize, limit - processed);
            // AsNoTracking 页取 + 逐批重查（词被并发修改时自然跳过，幂等由 AiStatus 守护）
            var page = await _db.Words.AsNoTracking()
                .Where(w => !w.Archived && w.AiStatus == 0
                    && (tag == null || w.Tag == tag))
                .OrderBy(w => w.Version)
                .Take(pageSize)
                .ToListAsync(ct);
            if (page.Count == 0) break;

            // 页内并发调 LLM（纯 I/O），DB 写入在收集后顺序执行
            var results = new (Guid Id, WordAiSuggestion? Sug)[page.Count];
            var tasks = page.Select(async (w, i) =>
            {
                await semaphore.WaitAsync(ct);
                try { results[i] = (w.Id, await _ai.GenerateAsync(w, contentKind, subject, ct)); }
                finally { semaphore.Release(); }
            });
            await Task.WhenAll(tasks);

            var okIds = results.Where(r => r.Sug is not null).Select(r => r.Id).ToHashSet();
            var tracked = await _db.Words.Where(w => okIds.Contains(w.Id)).ToListAsync(ct);
            foreach (var word in tracked)
            {
                var sug = results.First(r => r.Id == word.Id).Sug!;
                word.AiStatus = 1;
                word.AiSuggestion = JsonSerializer.Serialize(sug, JsonOpts);
            }
            // 失败词（含未跟踪到的竞态）标 3：不再反复重试同批，人工驳回可复位
            var failIds = page.Where(w => !okIds.Contains(w.Id)).Select(w => w.Id).ToList();
            if (failIds.Count > 0)
            {
                var failTracked = await _db.Words.Where(w => failIds.Contains(w.Id)).ToListAsync(ct);
                foreach (var word in failTracked) word.AiStatus = 3;
            }
            await _db.SaveChangesAsync(ct);

            processed += page.Count;
            failed += page.Count - okIds.Count;
            AddFails(page.Count - okIds.Count);
            _logger.LogInformation("AiContentJob(kind={Kind}) 批次完成：本轮 {Done} 条，失败 {Fail} 条",
                contentKind, page.Count, page.Count - okIds.Count);
        }

        _logger.LogInformation("AiContentJob(kind={Kind}) 结束：处理 {Processed} 条，失败 {Failed} 条",
            contentKind, processed, failed);
    }

    private bool ShouldBreak()
    {
        lock (BreakerLock)
        {
            if (_breakerDate != DateTime.UtcNow.Date)
            {
                _breakerDate = DateTime.UtcNow.Date;
                _breakerFails = 0;
            }
            if (_breakerFails >= DailyFailBreaker)
            {
                _logger.LogError("AiContentJob 熔断：当日失败已达 {N}，终止（次日自动恢复）", _breakerFails);
                return true;
            }
            return false;
        }
    }

    private void AddFails(int n)
    {
        lock (BreakerLock) _breakerFails += n;
    }
}
