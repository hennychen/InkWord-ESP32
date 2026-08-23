using System.Globalization;
using InkWord.Core.Entities;
using InkWord.Services;
using Xunit;

namespace InkWord.Tests;

/// <summary>
/// FsrsService 对拍测试（M3 路径 A，2026-08-22）。
///
/// 读 tools/fsrs_test_vectors.csv（gen_fsrs_vectors.py 独立参考实现生成的
/// 期望值），经 ApplyShadow 影子接口重放 12 组复习序列：S/D 相对容差
/// 1e-4，间隔整数精确相等。固件 test/test_srs_engine.c 读同一 CSV
/// （双端对拍纪律，向量改动须重跑生成脚本）。
/// </summary>
public class FsrsServiceTests
{
    private readonly FsrsService _fsrs = new();

    private sealed record Row(string Case, int Step, int Quality, int ElapsedDays,
        double S, double D, int Interval);

    private static List<Row> LoadRows()
    {
        var path = Path.Combine(AppContext.BaseDirectory, "fsrs_test_vectors.csv");
        if (!File.Exists(path))
            throw new FileNotFoundException(
                "测试向量缺失：运行 tools/gen_fsrs_vectors.py 重新生成", path);

        var rows = new List<Row>();
        var lines = File.ReadAllLines(path);
        foreach (var line in lines.Skip(1)) // 跳过表头
        {
            if (string.IsNullOrWhiteSpace(line)) continue;
            var f = line.Split(',');
            rows.Add(new Row(f[0],
                int.Parse(f[1]), int.Parse(f[2]), int.Parse(f[3]),
                double.Parse(f[4], CultureInfo.InvariantCulture),
                double.Parse(f[5], CultureInfo.InvariantCulture),
                int.Parse(f[6])));
        }
        return rows;
    }

    /// <summary>逐 case 重放：last_review 手动推进（影子接口不写 LastStudiedAt，
    /// 与 SrsService 中「先影子后 SM-2 覆写」的实际运行语义一致）。</summary>
    [Fact]
    public void ReplayAllCases_MatchReferenceVectors()
    {
        var baseTime = new DateTime(2026, 8, 22, 0, 0, 0, DateTimeKind.Utc);

        foreach (var group in LoadRows().GroupBy(r => r.Case))
        {
            var rec = new LearningRecord { FsrsStability = 0, FsrsDifficulty = 0 };
            rec.LastStudiedAt = baseTime;
            var lastReview = baseTime;

            foreach (var row in group.OrderBy(r => r.Step))
            {
                var now = lastReview.AddDays(row.ElapsedDays);
                _fsrs.ApplyShadow(rec, row.Quality, now);
                // 影子接口不写 LastStudiedAt（实际由 SM-2 主列覆写），
                // 重放时手动同步，否则后续步骤的 elapsed 会累加错天
                rec.LastStudiedAt = now;

                AssertRelative(row.S, rec.FsrsStability, $"{group.Key}[{row.Step}].S");
                AssertRelative(row.D, rec.FsrsDifficulty, $"{group.Key}[{row.Step}].D");
                Assert.Equal(row.Interval, (rec.FsrsNextReview!.Value - now).Days);
                lastReview = now;
            }
        }
    }

    private static void AssertRelative(double expected, double actual, string label)
    {
        var tol = Math.Max(1e-4, Math.Abs(expected) * 1e-4);
        Assert.True(Math.Abs(actual - expected) <= tol,
            $"{label}: expected={expected}, actual={actual}");
    }

    // ---- 锚点（公开已知事实，独立于参考实现）----

    [Fact]
    public void MapRating_FollowsQualityBands()
    {
        // 0-1→Again 2→Hard 3-4→Good 5→Easy（与 SM-2 按键语义对齐）
        Assert.Equal(1, FsrsService.MapRating(0));
        Assert.Equal(1, FsrsService.MapRating(1));
        Assert.Equal(2, FsrsService.MapRating(2));
        Assert.Equal(3, FsrsService.MapRating(3));
        Assert.Equal(3, FsrsService.MapRating(4));
        Assert.Equal(4, FsrsService.MapRating(5));
    }

    [Fact]
    public void FirstReview_InitializesSAndDFromW()
    {
        var rec = new LearningRecord();
        var now = DateTime.UtcNow;
        _fsrs.ApplyShadow(rec, 3, now);

        Assert.Equal(3.7145, rec.FsrsStability, 6); // S0(Good)=W[2]
        Assert.Equal(1.0, rec.FsrsDifficulty, 6);    // D0(Good)=clamp(5.1618-e^1.795)→1
        Assert.Equal(now.AddDays(4), rec.FsrsNextReview); // round(3.7145)=4
    }

    [Fact]
    public void NextInterval_AtDesiredRetention_EqualsRoundOfS()
    {
        // 目标留存 0.90 下 FACTOR 化简使 interval = round(S)（设计不变量）
        Assert.Equal(4, FsrsService.NextInterval(3.7145));
        Assert.Equal(23, FsrsService.NextInterval(22.716248));
        Assert.Equal(1, FsrsService.NextInterval(0.01)); // 下限保护
    }
}
