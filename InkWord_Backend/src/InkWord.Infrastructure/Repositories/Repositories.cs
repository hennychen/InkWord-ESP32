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
        // 自 localVersion 以来的变更，排除已归档，按 Version 升序分页。
        // 注意：不能按 Difficulty 排序 —— 设备分页游标是 version（取本批最大
        // version 作下批起点），难度序会使 Take 窗口与游标错位而跳词
        // （2026-08-23 实测 2407 词分 2 批拉取丢 347 词后修正）。
        // 附带效应：words.json 导出顺序 = Version 序（种子导入序），
        // 只要存量词条 Version 不变导出顺序即稳定，设备学习状态不受影响。
        return await DbContext.Words.AsNoTracking()
            .Where(w => w.Version > localVersion && !w.Archived)
            .OrderBy(w => w.Version)
            .Take(count)
            .ToListAsync(ct);
    }

    public async Task<int> GetMaxVersionAsync(CancellationToken ct = default)
        => await DbContext.Words.AsNoTracking().MaxAsync(w => (int?)w.Version, ct) ?? 0;
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
