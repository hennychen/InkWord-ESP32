using InkWord.API.Controllers;
using InkWord.API.DTOs;
using InkWord.Core.Entities;
using InkWord.Core.Repositories;
using InkWord.Services;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Mvc;
using Microsoft.Extensions.Configuration;

namespace InkWord.Tests;

/// <summary>
/// B-10c 墨封上报端点直测（2026-09-04 墨封功能）。
///
/// 手写替身直测（ChatServiceTests 的 IChatClient 替身同款先例，无 mock
/// 框架依赖）：DeviceAuthFilter 不参与（直测绕过鉴权，Items["Device"]
/// 直接注入）。断言口径与固件 learning_state.toggle_master 双端一致：
/// 置位清 ConsecutiveWrong / 启封不动 / 未学词落初始记录 / 幂等 set。
/// </summary>
public class SyncMasterTests
{
    private sealed class RecordRepoStub : ILearningRecordRepository
    {
        public LearningRecord? Rec;
        public int Added;

        public Task<LearningRecord?> GetAsync(Guid deviceId, Guid wordId,
            CancellationToken ct = default)
            => Task.FromResult(
                Rec is { } r && r.DeviceId == deviceId && r.WordId == wordId ? r : null);

        public Task<LearningRecord> AddAsync(LearningRecord entity,
            CancellationToken ct = default)
        {
            Rec = entity;
            Added++;
            return Task.FromResult(entity);
        }

        public Task<int> SaveChangesAsync(CancellationToken ct = default)
            => Task.FromResult(1);

        // 端点未触达路径
        public Task<IReadOnlyList<LearningRecord>> GetDueAsync(Guid deviceId, DateTime now,
            CancellationToken ct = default)
            => throw new NotSupportedException();
        public Task<LearningRecord?> GetByIdAsync(Guid id, CancellationToken ct = default)
            => throw new NotSupportedException();
        public Task<IReadOnlyList<LearningRecord>> ListAllAsync(CancellationToken ct = default)
            => throw new NotSupportedException();
        public Task<IReadOnlyList<LearningRecord>> FindByConditionAsync(
            System.Linq.Expressions.Expression<Func<LearningRecord, bool>> predicate,
            CancellationToken ct = default)
            => throw new NotSupportedException();
        public Task<(IReadOnlyList<LearningRecord> Items, int Total)> GetPagedAsync(
            int page, int size,
            System.Linq.Expressions.Expression<Func<LearningRecord, bool>>? predicate = null,
            CancellationToken ct = default)
            => throw new NotSupportedException();
        public Task UpdateAsync(LearningRecord entity, CancellationToken ct = default)
            => throw new NotSupportedException();
        public Task DeleteAsync(LearningRecord entity, CancellationToken ct = default)
            => throw new NotSupportedException();
    }

    private static (DeviceController Ctrl, Device Dev) NewController(RecordRepoStub repo)
    {
        var ctrl = new DeviceController(null!, null!, repo, null!,
            new SrsService(new FsrsService(), new ConfigurationBuilder().Build()),
            null!, null!, null!, null!, null!, null!);
        var dev = new Device { Id = Guid.NewGuid() };
        var http = new DefaultHttpContext();
        http.Items["Device"] = dev;
        ctrl.ControllerContext = new ControllerContext { HttpContext = http };
        return (ctrl, dev);
    }

    [Fact]
    public async Task Master_ClearsConsecutiveWrong_IdempotentSet()
    {
        var repo = new RecordRepoStub
        {
            Rec = new LearningRecord { ConsecutiveWrong = 3 },
        };
        var (ctrl, dev) = NewController(repo);
        repo.Rec!.DeviceId = dev.Id;
        var word = Guid.NewGuid();
        repo.Rec.WordId = word;

        var r = await ctrl.SyncMaster(new MasterReq(word, true), default);
        Assert.IsType<OkObjectResult>(r);
        Assert.True(repo.Rec.IsMastered);
        Assert.Equal(0, repo.Rec.ConsecutiveWrong);    // 声明式通过清连错（双端同规则）

        // 幂等重复上报同值：状态稳定不清错不清调度
        await ctrl.SyncMaster(new MasterReq(word, true), default);
        Assert.True(repo.Rec.IsMastered);
        Assert.Equal(0, repo.Rec.ConsecutiveWrong);
    }

    [Fact]
    public async Task Unmaster_KeepsConsecutiveWrong()
    {
        var repo = new RecordRepoStub
        {
            Rec = new LearningRecord { IsMastered = true, ConsecutiveWrong = 2 },
        };
        var (ctrl, dev) = NewController(repo);
        repo.Rec.DeviceId = dev.Id;
        var word = Guid.NewGuid();
        repo.Rec.WordId = word;

        await ctrl.SyncMaster(new MasterReq(word, false), default);

        Assert.False(repo.Rec.IsMastered);
        Assert.Equal(2, repo.Rec.ConsecutiveWrong);    // 启封不动连错（正负不对称）
    }

    [Fact]
    public async Task Master_UnknownWord_CreatesInitialRecord()
    {
        var repo = new RecordRepoStub();
        var (ctrl, _) = NewController(repo);
        var word = Guid.NewGuid();

        await ctrl.SyncMaster(new MasterReq(word, true), default);

        Assert.Equal(1, repo.Added);                   // 未学词落初始记录（收藏同款先例）
        Assert.NotNull(repo.Rec);
        Assert.Equal(word, repo.Rec!.WordId);
        Assert.True(repo.Rec.IsMastered);
        Assert.Equal(0, repo.Rec.ConsecutiveWrong);
    }
}
