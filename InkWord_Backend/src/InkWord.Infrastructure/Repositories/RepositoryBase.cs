using System.Linq.Expressions;
using InkWord.Core.Common;
using InkWord.Core.Repositories;
using InkWord.Infrastructure.DbContext;
using Microsoft.EntityFrameworkCore;

namespace InkWord.Infrastructure.Repositories;

/// <summary>
/// 泛型仓储基类实现，含 GetPagedAsync / FindByConditionAsync 等扩展方法。
/// </summary>
public abstract class RepositoryBase<T> : IRepository<T> where T : BaseEntity
{
    protected readonly AppDbContext DbContext;
    protected readonly DbSet<T> DbSet;

    protected RepositoryBase(AppDbContext context)
    {
        DbContext = context;
        DbSet = context.Set<T>();
    }

    public virtual async Task<T?> GetByIdAsync(Guid id, CancellationToken ct = default)
        => await DbSet.FirstOrDefaultAsync(e => e.Id == id, ct);

    public virtual async Task<IReadOnlyList<T>> ListAllAsync(CancellationToken ct = default)
        => await DbSet.AsNoTracking().ToListAsync(ct);

    public virtual async Task<IReadOnlyList<T>> FindByConditionAsync(
        Expression<Func<T, bool>> predicate, CancellationToken ct = default)
        => await DbSet.AsNoTracking().Where(predicate).ToListAsync(ct);

    public virtual async Task<(IReadOnlyList<T> Items, int Total)> GetPagedAsync(
        int page, int size, Expression<Func<T, bool>>? predicate = null, CancellationToken ct = default)
    {
        var query = DbSet.AsNoTracking();
        if (predicate != null) query = query.Where(predicate);

        var total = await query.CountAsync(ct);
        var items = await query.OrderByDescending(e => e.CreatedAt)
                               .Skip((page - 1) * size)
                               .Take(size)
                               .ToListAsync(ct);
        return (items, total);
    }

    public virtual async Task<T> AddAsync(T entity, CancellationToken ct = default)
    {
        await DbSet.AddAsync(entity, ct);
        return entity;
    }

    public virtual Task UpdateAsync(T entity, CancellationToken ct = default)
    {
        entity.UpdatedAt = DateTime.UtcNow;
        DbContext.Entry(entity).State = EntityState.Modified;
        return Task.CompletedTask;
    }

    public virtual Task DeleteAsync(T entity, CancellationToken ct = default)
    {
        entity.IsDeleted = true;
        entity.UpdatedAt = DateTime.UtcNow;
        return Task.CompletedTask;
    }

    public virtual Task<int> SaveChangesAsync(CancellationToken ct = default)
        => DbContext.SaveChangesAsync(ct);
}
