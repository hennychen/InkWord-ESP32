using InkWord.API.DTOs;
using InkWord.Core.Common;
using InkWord.Infrastructure.DbContext;
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Mvc;
using Microsoft.EntityFrameworkCore;

namespace InkWord.API.Controllers;

/// <summary>数据看板统计（B-18）。</summary>
[ApiController]
[Route("api/admin/dashboard")]
[Authorize(Roles = "Admin,Operator")]
public class AdminDashboardController : ControllerBase
{
    private readonly AppDbContext _db;
    public AdminDashboardController(AppDbContext db) => _db = db;

    [HttpGet("stats")]
    public async Task<IActionResult> Stats(CancellationToken ct)
    {
        var now = DateTime.UtcNow;
        var todayStart = now.Date;

        var totalWords = await _db.Words.AsNoTracking().CountAsync(ct);
        var totalDevices = await _db.Devices.AsNoTracking().CountAsync(ct);
        var onlineDevices = await _db.Devices.AsNoTracking()
            .CountAsync(d => d.LastHeartbeat > now.AddMinutes(-5), ct);
        var activeToday = await _db.LearningRecords.AsNoTracking()
            .Where(r => r.LastStudiedAt >= todayStart)
            .Select(r => r.DeviceId).Distinct().CountAsync(ct);

        // SRS 等级分布
        var srsDist = await _db.LearningRecords.AsNoTracking()
            .Where(r => !r.Device!.IsDeleted)
            .GroupBy(r => r.SrsLevel)
            .Select(g => new SrsDistributionItem(g.Key, g.Count()))
            .ToListAsync(ct);

        // 近 7 天日活（按学习记录）
        var weekStart = now.AddDays(-6).Date;
        var dailyActive = await _db.LearningRecords.AsNoTracking()
            .Where(r => r.LastStudiedAt >= weekStart)
            .GroupBy(r => r.LastStudiedAt.Date)
            .Select(g => new DailyActiveItem(g.Key, g.Select(r => r.DeviceId).Distinct().Count()))
            .ToListAsync(ct);

        var stats = new DashboardStats(
            totalWords, totalDevices, onlineDevices, activeToday,
            AvgStudyMinutes: 0, srsDist, dailyActive);

        return Ok(ApiResponse<DashboardStats>.Ok(stats));
    }

    /// <summary>今日学习统计（v1.3 T3.2）：LearningRecord 按日聚合，
    /// 口径近似见 TodayStatsResp 注释；无记录时返回全 0。</summary>
    [HttpGet("today-stats")]
    public async Task<IActionResult> TodayStats(CancellationToken ct)
    {
        var now = DateTime.UtcNow;
        var todayStart = now.Date;

        var agg = await _db.LearningRecords.AsNoTracking()
            .Where(r => r.LastStudiedAt >= todayStart)
            .GroupBy(_ => 1)
            .Select(g => new
            {
                ActiveDevices = g.Select(r => r.DeviceId).Distinct().Count(),
                Touched = g.Count(),
                NewWords = g.Count(r => r.ReviewCount == 1),
                Reviews = g.Count(r => r.ReviewCount > 1),
                Correct = g.Count(r => r.LastQuality >= 3),
                Wrong = g.Count(r => r.LastQuality < 3),
                AvgQ = g.Average(r => (double?)r.LastQuality) ?? 0,
            })
            .FirstOrDefaultAsync(ct);

        var resp = agg == null
            ? new TodayStatsResp(0, 0, 0, 0, 0, 0, 0)
            : new TodayStatsResp(agg.ActiveDevices, agg.Touched, agg.NewWords,
                agg.Reviews, agg.Correct, agg.Wrong, Math.Round(agg.AvgQ, 2));

        return Ok(ApiResponse<TodayStatsResp>.Ok(resp));
    }

    /// <summary>错词排行（P1 错词本）：ConsecutiveWrong&gt;0 聚合，
    /// 按总连错人次降序；附带收藏记录数。</summary>
    [HttpGet("wrong-top")]
    public async Task<IActionResult> WrongTop([FromQuery] int top, CancellationToken ct)
    {
        top = top <= 0 || top > 100 ? 20 : top;

        // 注意：GroupBy 后直接投影 record 再 OrderBy 会翻译失败
        // （EF Core 8 无法把 record 成员映射回 SUM 聚合列，2026-08-21 实测），
        // 故先投影匿名类型完成排序/Take，最后一步再构造 record。
        var items = await _db.LearningRecords.AsNoTracking()
            .Where(r => r.ConsecutiveWrong > 0)
            .Join(_db.Words.AsNoTracking(),
                  r => r.WordId, w => w.Id,
                  (r, w) => new { w.Text, w.Meaning, r.ConsecutiveWrong })
            .GroupBy(x => new { x.Text, x.Meaning })
            .Select(g => new
            {
                g.Key.Text, g.Key.Meaning,
                WrongCount = g.Sum(x => x.ConsecutiveWrong),
                Learners = g.Count()
            })
            .OrderByDescending(x => x.WrongCount)
            .Take(top)
            .Select(x => new WrongTopItem(x.Text, x.Meaning, x.WrongCount, x.Learners))
            .ToListAsync(ct);

        var collected = await _db.LearningRecords.AsNoTracking()
            .CountAsync(r => r.IsCollected, ct);
        var mastered = await _db.LearningRecords.AsNoTracking()
            .CountAsync(r => r.IsMastered, ct);   // 墨封（2026-09-04）

        return Ok(ApiResponse<WrongTopResp>.Ok(new WrongTopResp(items, collected, mastered)));
    }

    public record SrsLevelDto(string Level, int Count);
    public record DailyActiveDto(string Date, int Count);

    /// <summary>SRS 分布（看板饼图）：等级分桶转语义名，
    /// 与前端 SrsDistribution { level: string; count: number } 对齐。</summary>
    [HttpGet("srs-distribution")]
    public async Task<IActionResult> SrsDistribution(CancellationToken ct)
    {
        var levels = await _db.LearningRecords.AsNoTracking()
            .Where(r => !r.Device!.IsDeleted)
            .GroupBy(r => r.SrsLevel)
            .Select(g => new { Level = g.Key, Count = g.Count() })
            .ToListAsync(ct);

        static string Name(int lv) => lv switch
        {
            0 => "新词",
            <= 2 => "学习中",
            <= 4 => "巩固中",
            _ => "已掌握",
        };

        var items = levels.GroupBy(g => Name(g.Level))
            .Select(g => new SrsLevelDto(g.Key, g.Sum(x => x.Count)))
            .OrderBy(i => i.Level)
            .ToList();

        return Ok(ApiResponse<List<SrsLevelDto>>.Ok(items));
    }

    /// <summary>SRS 算法对比（M3.3 路径 A）：按到期时间窗聚合 SM-2 主列
    /// 与 FSRS 影子列的排期差异（今日/本周/本月/更远 + 平均间隔）。
    /// FSRS 口径平均间隔取平均稳定性 S（目标留存 0.90 下 interval=round(S)）。</summary>
    [HttpGet("srs-comparison")]
    public async Task<IActionResult> SrsComparison(CancellationToken ct)
    {
        var now = DateTime.UtcNow;
        var week = now.AddDays(7);
        var month = now.AddDays(30);

        var agg = await _db.LearningRecords.AsNoTracking()
            .Where(r => !r.Device!.IsDeleted)
            .GroupBy(_ => 1)
            .Select(g => new
            {
                Sm2N = g.Count(r => r.ReviewCount > 0),
                Sm2Today = g.Count(r => r.ReviewCount > 0 && r.NextReview <= now),
                Sm2Week = g.Count(r => r.ReviewCount > 0 && r.NextReview > now && r.NextReview <= week),
                Sm2Month = g.Count(r => r.ReviewCount > 0 && r.NextReview > week && r.NextReview <= month),
                Sm2Future = g.Count(r => r.ReviewCount > 0 && r.NextReview > month),
                Sm2Avg = g.Where(r => r.ReviewCount > 0).Average(r => (double?)r.IntervalDays) ?? 0,

                FsrsN = g.Count(r => r.FsrsNextReview != null),
                FsrsToday = g.Count(r => r.FsrsNextReview != null && r.FsrsNextReview <= now),
                FsrsWeek = g.Count(r => r.FsrsNextReview != null && r.FsrsNextReview > now && r.FsrsNextReview <= week),
                FsrsMonth = g.Count(r => r.FsrsNextReview != null && r.FsrsNextReview > week && r.FsrsNextReview <= month),
                FsrsFuture = g.Count(r => r.FsrsNextReview != null && r.FsrsNextReview > month),
                FsrsAvg = g.Where(r => r.FsrsNextReview != null).Average(r => (double?)r.FsrsStability) ?? 0,
            })
            .FirstOrDefaultAsync(ct);

        if (agg == null)
            return Ok(ApiResponse<SrsComparisonResp>.Ok(new SrsComparisonResp(0, 0, [])));

        var items = new List<SrsComparisonItem>
        {
            new("sm2", agg.Sm2Today, agg.Sm2Week, agg.Sm2Month, agg.Sm2Future, Math.Round(agg.Sm2Avg, 1)),
            new("fsrs", agg.FsrsToday, agg.FsrsWeek, agg.FsrsMonth, agg.FsrsFuture, Math.Round(agg.FsrsAvg, 1)),
        };
        return Ok(ApiResponse<SrsComparisonResp>.Ok(new SrsComparisonResp(agg.Sm2N, agg.FsrsN, items)));
    }

    /// <summary>日活趋势（看板折线）：近 N 天每日活跃设备数，
    /// 缺日补 0，date 格式 yyyy-MM-dd。</summary>
    [HttpGet("daily-active")]
    public async Task<IActionResult> DailyActive([FromQuery] int days, CancellationToken ct)
    {
        days = days <= 0 || days > 90 ? 30 : days;
        var today = DateTime.UtcNow.Date;
        var start = today.AddDays(-(days - 1));

        var byDate = await _db.LearningRecords.AsNoTracking()
            .Where(r => r.LastStudiedAt >= start)
            .GroupBy(r => r.LastStudiedAt.Date)
            .Select(g => new { Date = g.Key, Count = g.Select(r => r.DeviceId).Distinct().Count() })
            .ToListAsync(ct);

        var map = byDate.ToDictionary(x => x.Date, x => x.Count);
        var items = Enumerable.Range(0, days)
            .Select(i => today.AddDays(-i))
            .OrderBy(d => d)
            .Select(d => new DailyActiveDto(d.ToString("yyyy-MM-dd"),
                map.TryGetValue(d, out var c) ? c : 0))
            .ToList();

        return Ok(ApiResponse<List<DailyActiveDto>>.Ok(items));
    }
}
