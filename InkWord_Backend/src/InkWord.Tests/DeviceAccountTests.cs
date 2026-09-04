using InkWord.API.Controllers;

namespace InkWord.Tests;

/// <summary>
/// v2.0 完整账户纯逻辑测试（ACCOUNT_MODEL_DECISION §五）。
///
/// LWS 归并（跨设备按 WordId 取 LastStudiedAt 新者整行胜出——FSRS 状态
/// 不可拆分合并红线）/ ApiKey 换发格式。绑定写库语义（幂等不重复换发/
/// 409/换发即旧钥失效）依赖数据库与设备 401 自愈链路，沿 T5.3 已知缺口
/// 口径留集成/真机验证。
/// </summary>
public class DeviceAccountTests
{
    private static MyDeviceController.LwsRow Row(Guid word, Guid device,
        DateTime studied, double stability = 1.0, bool collected = false,
        bool mastered = false) =>
        new(word, device, stability, 2.5, null, collected, mastered, studied);

    // ---- LWS 归并 ----

    [Fact]
    public void LwsMerge_LaterStudyWins_WholeRow()
    {
        var w = Guid.NewGuid();
        var d1 = Guid.NewGuid();
        var d2 = Guid.NewGuid();
        var t1 = new DateTime(2026, 8, 20, 0, 0, 0, DateTimeKind.Utc);
        var t2 = new DateTime(2026, 8, 25, 0, 0, 0, DateTimeKind.Utc);

        // 乱序输入：归并自带防御性排序（直测入口不保证有序）
        var r = MyDeviceController.LwsMerge([
            Row(w, d1, t1, stability: 3.0, collected: true),
            Row(w, d2, t2, stability: 9.9),
        ], 10);

        var only = Assert.Single(r);
        Assert.Equal(d2, only.DeviceId);      // 新者胜
        Assert.Equal(9.9, only.Stability);    // 整行取自胜者（不跨设备混合）
        Assert.False(only.IsCollected);       // 旧设备的收藏不渗入
    }

    [Fact]
    public void LwsMerge_DistinctWords_AllKept_RecentFirst()
    {
        var d = Guid.NewGuid();
        var w1 = Guid.NewGuid();
        var w2 = Guid.NewGuid();
        var w3 = Guid.NewGuid();
        var base_ = new DateTime(2026, 8, 25, 0, 0, 0, DateTimeKind.Utc);

        var r = MyDeviceController.LwsMerge([
            Row(w1, d, base_.AddHours(1)),
            Row(w2, d, base_.AddHours(3)),
            Row(w3, d, base_.AddHours(2)),
        ], 10);

        Assert.Equal(3, r.Count);
        Assert.Equal([w2, w3, w1], r.Select(x => x.WordId).ToList());
    }

    [Fact]
    public void LwsMerge_TakeClamps_OutputSize()
    {
        var d = Guid.NewGuid();
        var base_ = new DateTime(2026, 8, 25, 0, 0, 0, DateTimeKind.Utc);
        var rows = Enumerable.Range(0, 10)
            .Select(i => Row(Guid.NewGuid(), d, base_.AddHours(i)))
            .ToList();

        Assert.Equal(3, MyDeviceController.LwsMerge(rows, 3).Count);
        // 截断保留最近学的
        var top = MyDeviceController.LwsMerge(rows, 1).Single();
        Assert.Equal(base_.AddHours(9), top.LastStudiedAt);
    }

    [Fact]
    public void LwsMerge_EmptyInput_EmptyOutput()
        => Assert.Empty(MyDeviceController.LwsMerge([], 10));

    // ---- ApiKey 换发 ----

    [Fact]
    public void GenerateApiKey_48HexLower_Unique()
    {
        var a = DeviceController.GenerateApiKey();
        var b = DeviceController.GenerateApiKey();
        Assert.Equal(48, a.Length);
        Assert.Matches("^[0-9a-f]{48}$", a);
        Assert.NotEqual(a, b);
    }
}
