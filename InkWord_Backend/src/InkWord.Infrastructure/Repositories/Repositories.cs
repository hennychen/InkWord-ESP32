using InkWord.Core.Entities;
using InkWord.Core.Repositories;
using InkWord.Infrastructure.DbContext;
using Microsoft.EntityFrameworkCore;

namespace InkWord.Infrastructure.Repositories;

public class WordRepository : RepositoryBase<Word>, IWordRepository
{
    public WordRepository(AppDbContext context) : base(context) { }

    public Task<bool> ExistsByTextAsync(string text, string? tag, CancellationToken ct = default)
    {
        var query = DbContext.Words.AsNoTracking().Where(w => w.Text == text);
        if (!string.IsNullOrEmpty(tag)) query = query.Where(w => w.Tag == tag);
        return query.AnyAsync(ct);
    }

    public async Task<IReadOnlyList<Word>> GetIncrementalAsync(int localVersion, int count, CancellationToken ct = default)
    {
        // 自 localVersion 以来的变更，按 Version 升序分页。
        // 注意：不能按 Difficulty 排序 —— 设备分页游标是 version（取本批最大
        // version 作下批起点），难度序会使 Take 窗口与游标错位而跳词
        // （2026-08-23 实测 2407 词分 2 批拉取丢 347 词后修正）。
        // 附带效应：words.json 导出顺序 = Version 序（种子导入序），
        // 只要存量词条 Version 不变导出顺序即稳定，设备学习状态不受影响。
        //
        // 协议 v3（2026-09-08 删除通道）：不再排除归档行——归档 = 删除，
        // 删除时 Version 接全局 max 递增，墓碑行随增量自然下发
        // （SyncWords 投影为 ChangeType=2 + 内容空，仅保留 text/tag 身份键）。
        // 历史归档（Version ≤ 设备游标）不下发——v3 前的删除设备侧不可见，
        // 需重新导出词库或 LAN 重推覆盖（存量语义边界，协议文档 §3.1）。
        return await DbContext.Words.AsNoTracking()
            .Where(w => w.Version > localVersion)
            .OrderBy(w => w.Version)
            .Take(count)
            .ToListAsync(ct);
    }

    public async Task<int> GetMaxVersionAsync(CancellationToken ct = default)
        => await DbContext.Words.AsNoTracking().MaxAsync(w => (int?)w.Version, ct) ?? 0;

    /// <summary>A5 竞态根治：委托给 VersionSequencer（应用层锁 + 事务内重读 max）。</summary>
    public Task<int> GetNextVersionAsync(int currentVersion, CancellationToken ct = default)
        => VersionSequencer.GetNextAsync(DbContext, currentVersion, ct);
}

public class DeviceRepository : RepositoryBase<Device>, IDeviceRepository
{
    public DeviceRepository(AppDbContext context) : base(context) { }

    public Task<Device?> GetByApiKeyAsync(string apiKey, CancellationToken ct = default)
        => DbContext.Devices.FirstOrDefaultAsync(d => d.ApiKey == apiKey, ct);

    public Task<Device?> GetByMacAsync(string mac, CancellationToken ct = default)
        => DbContext.Devices.FirstOrDefaultAsync(d => d.MacAddress == mac, ct);
}

public class UserRepository : RepositoryBase<User>, IUserRepository
{
    public UserRepository(AppDbContext context) : base(context) { }

    public Task<User?> GetByUsernameAsync(string username, CancellationToken ct = default)
        => DbContext.Users.FirstOrDefaultAsync(u => u.Username == username, ct);
}

public class AccountRepository : RepositoryBase<Account>, IAccountRepository
{
    public AccountRepository(AppDbContext context) : base(context) { }

    public Task<Account?> GetByUsernameAsync(string username, CancellationToken ct = default)
        => DbContext.Accounts.FirstOrDefaultAsync(a => a.Username == username, ct);
}

public class LearningRecordRepository : RepositoryBase<LearningRecord>, ILearningRecordRepository
{
    public LearningRecordRepository(AppDbContext context) : base(context) { }

    public Task<LearningRecord?> GetAsync(Guid deviceId, Guid wordId, CancellationToken ct = default)
        => DbContext.LearningRecords.FirstOrDefaultAsync(r => r.DeviceId == deviceId && r.WordId == wordId, ct);

    public async Task<IReadOnlyList<LearningRecord>> GetDueAsync(Guid deviceId, DateTime now, CancellationToken ct = default)
        => await DbContext.LearningRecords.AsNoTracking()
            .Where(r => r.DeviceId == deviceId && r.NextReview <= now)
            .OrderBy(r => r.NextReview)
            .ToListAsync(ct);
}

public class OtaPackageRepository : RepositoryBase<OtaPackage>, IOtaPackageRepository
{
    public OtaPackageRepository(AppDbContext context) : base(context) { }

    public async Task<OtaPackage?> GetLatestPublishedAsync(string targetBoard, CancellationToken ct = default)
        => await DbContext.OtaPackages.AsNoTracking()
            .Where(o => o.Published && o.TargetBoard == targetBoard)
            .OrderByDescending(o => o.CreatedAt)
            .FirstOrDefaultAsync(ct);
}

// ====== 阅读器后端（2026-09-05） ======

public class BookRepository : RepositoryBase<Book>, IBookRepository
{
    public BookRepository(AppDbContext context) : base(context) { }

    public Task<Book?> GetByBookKeyAsync(string bookKey, CancellationToken ct = default)
        => DbContext.Books.FirstOrDefaultAsync(b => b.BookKey == bookKey, ct);

    public async Task<IReadOnlyList<Book>> GetPublishedAsync(CancellationToken ct = default)
        => await DbContext.Books.AsNoTracking()
            .Where(b => b.Published)
            .OrderBy(b => b.Title)
            .ToListAsync(ct);

    public async Task<(IReadOnlyList<Book> Items, int Total)> QueryAsync(
        string? keyword, string? language, bool? published, int page, int size,
        CancellationToken ct = default)
    {
        var q = DbContext.Books.AsNoTracking().Where(b => !b.IsDeleted);
        if (!string.IsNullOrEmpty(keyword))
            q = q.Where(b => b.Title.Contains(keyword) || b.Author.Contains(keyword) || b.BookKey.Contains(keyword));
        if (!string.IsNullOrEmpty(language))
            q = q.Where(b => b.Language == language);
        if (published.HasValue)
            q = q.Where(b => b.Published == published.Value);

        var total = await q.CountAsync(ct);
        var items = await q.OrderBy(b => b.Title)
            .Skip((page - 1) * size).Take(size)
            .ToListAsync(ct);
        return (items, total);
    }
}

public class ReadingProgressRepository : RepositoryBase<ReadingProgress>, IReadingProgressRepository
{
    public ReadingProgressRepository(AppDbContext context) : base(context) { }

    public Task<ReadingProgress?> GetAsync(Guid deviceId, Guid bookId, CancellationToken ct = default)
        => DbContext.ReadingProgresses.FirstOrDefaultAsync(r => r.DeviceId == deviceId && r.BookId == bookId, ct);

    public async Task<IReadOnlyList<ReadingProgress>> GetByDeviceAsync(Guid deviceId, CancellationToken ct = default)
        => await DbContext.ReadingProgresses.AsNoTracking()
            .Where(r => r.DeviceId == deviceId)
            .OrderByDescending(r => r.LastReadAt)
            .ToListAsync(ct);
}

public class DeviceBookmarkRepository : RepositoryBase<DeviceBookmark>, IDeviceBookmarkRepository
{
    public DeviceBookmarkRepository(AppDbContext context) : base(context) { }

    public async Task<IReadOnlyList<DeviceBookmark>> GetListAsync(Guid deviceId, Guid bookId, CancellationToken ct = default)
        => await DbContext.DeviceBookmarks.AsNoTracking()
            .Where(b => b.DeviceId == deviceId && b.BookId == bookId)
            .OrderBy(b => b.Page)
            .ToListAsync(ct);

    public async Task DeleteByBookAsync(Guid deviceId, Guid bookId, CancellationToken ct = default)
    {
        var marks = await DbContext.DeviceBookmarks
            .Where(b => b.DeviceId == deviceId && b.BookId == bookId)
            .ToListAsync(ct);
        DbContext.DeviceBookmarks.RemoveRange(marks);
    }
}
