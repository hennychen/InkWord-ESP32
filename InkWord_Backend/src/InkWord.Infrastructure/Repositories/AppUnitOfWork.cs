using InkWord.Infrastructure.DbContext;

namespace InkWord.Infrastructure.Repositories;

/// <summary>
/// 工作单元实现：持有 Scoped AppDbContext，统一 SaveChanges。
/// </summary>
public sealed class AppUnitOfWork : IUnitOfWork
{
    public AppUnitOfWork(AppDbContext db) => Db = db;

    public AppDbContext Db { get; }

    public Task<int> SaveChangesAsync(CancellationToken ct = default)
        => Db.SaveChangesAsync(ct);

    /// <summary>A5 竞态根治：委托给 VersionSequencer（应用层锁 + 事务内重读 max）。</summary>
    public Task<int> GetNextVersionAsync(int currentVersion, CancellationToken ct = default)
        => VersionSequencer.GetNextAsync(Db, currentVersion, ct);

    public void Dispose() => Db.Dispose();
}
