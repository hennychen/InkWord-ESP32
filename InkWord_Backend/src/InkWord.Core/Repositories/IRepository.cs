using System.Linq.Expressions;
using InkWord.Core.Common;

namespace InkWord.Core.Repositories;

/// <summary>
/// 泛型仓储基类接口。
/// </summary>
public interface IRepository<T> where T : BaseEntity
{
    Task<T?> GetByIdAsync(Guid id, CancellationToken ct = default);
    Task<IReadOnlyList<T>> ListAllAsync(CancellationToken ct = default);

    /// <summary>按条件查询</summary>
    Task<IReadOnlyList<T>> FindByConditionAsync(
        Expression<Func<T, bool>> predicate, CancellationToken ct = default);

    /// <summary>分页查询</summary>
    Task<(IReadOnlyList<T> Items, int Total)> GetPagedAsync(
        int page, int size,
        Expression<Func<T, bool>>? predicate = null,
        CancellationToken ct = default);

    Task<T> AddAsync(T entity, CancellationToken ct = default);
    Task UpdateAsync(T entity, CancellationToken ct = default);
    Task DeleteAsync(T entity, CancellationToken ct = default);   // 软删除

    Task<int> SaveChangesAsync(CancellationToken ct = default);
}
