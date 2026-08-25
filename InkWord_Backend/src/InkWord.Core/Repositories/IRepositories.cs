using InkWord.Core.Entities;

namespace InkWord.Core.Repositories;

public interface IWordRepository : IRepository<Word>
{
    /// <summary>检查单词文本是否已存在（去重校验）</summary>
    Task<bool> ExistsByTextAsync(string text, string? tag, CancellationToken ct = default);

    /// <summary>获取自 localVersion 以来的增量变更（按 SRS 优先级排序）</summary>
    Task<IReadOnlyList<Word>> GetIncrementalAsync(int localVersion, int count, CancellationToken ct = default);

    /// <summary>当前最大词库版本号</summary>
    Task<int> GetMaxVersionAsync(CancellationToken ct = default);
}

public interface IDeviceRepository : IRepository<Device>
{
    Task<Device?> GetByApiKeyAsync(string apiKey, CancellationToken ct = default);
    Task<Device?> GetByMacAsync(string mac, CancellationToken ct = default);
}

public interface IUserRepository : IRepository<User>
{
    Task<User?> GetByUsernameAsync(string username, CancellationToken ct = default);
}

/// <summary>
/// 学习者账户仓储（v1.5 T5.3 轻账户，与 User 分表）。
/// </summary>
public interface IAccountRepository : IRepository<Account>
{
    Task<Account?> GetByUsernameAsync(string username, CancellationToken ct = default);
}

public interface ILearningRecordRepository : IRepository<LearningRecord>
{
    Task<LearningRecord?> GetAsync(Guid deviceId, Guid wordId, CancellationToken ct = default);
    Task<IReadOnlyList<LearningRecord>> GetDueAsync(Guid deviceId, DateTime now, CancellationToken ct = default);
}

public interface IOtaPackageRepository : IRepository<OtaPackage>
{
    Task<OtaPackage?> GetLatestPublishedAsync(string targetBoard, CancellationToken ct = default);
}
