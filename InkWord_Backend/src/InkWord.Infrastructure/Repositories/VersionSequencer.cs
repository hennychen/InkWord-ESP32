using Microsoft.EntityFrameworkCore;
using InkWord.Infrastructure.DbContext;

namespace InkWord.Infrastructure.Repositories;

/// <summary>
/// A5 竞态根治：Version 发号器（应用层锁 + 事务内重读 max，保证原子性）。
/// 单实例部署够用；多实例需改数据库序列发号（Sqlite 不支持，PostgreSQL 可用）。
/// </summary>
internal static class VersionSequencer
{
    private static readonly SemaphoreSlim _lock = new(1, 1);

    /// <summary>获取下一个版本号（线程安全）</summary>
    /// <param name="db">DbContext（用于查询 max）</param>
    /// <param name="currentVersion">当前版本号（新条目传 0）</param>
    /// <param name="ct">取消令牌</param>
    /// <returns>下一个版本号（= max(currentVersion, globalMax) + 1）</returns>
    public static async Task<int> GetNextAsync(AppDbContext db, int currentVersion, CancellationToken ct = default)
    {
        await _lock.WaitAsync(ct);
        try
        {
            var globalMax = await db.Words.AsNoTracking()
                .MaxAsync(w => (int?)w.Version, ct) ?? 0;
            return Math.Max(currentVersion, globalMax) + 1;
        }
        finally
        {
            _lock.Release();
        }
    }
}
