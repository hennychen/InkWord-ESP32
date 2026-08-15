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
[Authorize]
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
            .Where(r => !r.Device.IsDeleted)
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
}
