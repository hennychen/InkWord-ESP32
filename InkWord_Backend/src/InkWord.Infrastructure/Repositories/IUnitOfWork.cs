using InkWord.Infrastructure.DbContext;

namespace InkWord.Infrastructure.Repositories;

/// <summary>
/// 工作单元接口：统一数据访问入口。
/// 写操作走仓储（AddAsync/UpdateAsync/DeleteAsync + SaveChangesAsync），
/// 复杂只读聚合经 Db 属性访问 DbSet（控制器不应通过 Db 执行写操作）。
/// </summary>
public interface IUnitOfWork : IDisposable
{
    /// <summary>底层 DbContext（只读查询用；写操作须走仓储）</summary>
    AppDbContext Db { get; }

    Task<int> SaveChangesAsync(CancellationToken ct = default);

    /// <summary>A5 竞态根治：获取下一个版本号（事务内重读 max，保证原子性）</summary>
    Task<int> GetNextVersionAsync(int currentVersion, CancellationToken ct = default);
}
