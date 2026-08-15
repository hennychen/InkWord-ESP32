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
        // 自 localVersion 以来的变更，排除已归档，按难度升序（简单词优先）
        return await DbContext.Words.AsNoTracking()
            .Where(w => w.Version > localVersion && !w.Archived)
            .OrderBy(w => w.Difficulty)
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
