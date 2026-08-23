using InkWord.Core.Entities;

namespace InkWord.Services;

/// <summary>
/// FSRS-4.5 间隔重复调度器（M3 路径 A，2026-08-22）。
///
/// 按 open-spaced-repetition 官方算法移植（17 参数默认权重），纯函数无状态，
/// 与固件 srs_engine.c 保持同源镜像——双端共用 tools/fsrs_test_vectors.csv
/// 对拍（容差：S/D 相对 1e-4，间隔精确相等）。quality 0~5 → rating 映射：
/// 0-1→Again 2→Hard 3-4→Good 5→Easy（与 SM-2 按键语义对齐）。
///
/// 公式（w 为 0 基下标）：
///   S0(r) = w[r-1]；D0(r) = w4 - e^(w5·(r-1))，clamp[1,10]
///   R(t,S) = (1 + FACTOR·t/S)^DECAY，DECAY=-0.5，FACTOR=19/81
///   回忆( r≥2 )：S' = S·(1 + e^w8·(11-D)·S^(-w9)·(e^(w10·(1-R))-1)·HP·EB)
///     HP = w15 (r=Hard)，EB = w16 (r=Easy)
///   遗忘( r=1 )：S' = w11·D^(-w12)·((S+1)^w13 - 1)·e^(w14·(1-R))
///   D' = clamp(D - w6·(r-3), 1, 10)
///   间隔 = round(S·(R*^(1/DECAY)-1)/FACTOR)，目标留存率 R*=0.90 时恰为 round(S)
/// </summary>
public class FsrsService
{
    // FSRS-4.5 默认权重（open-spaced-repetition 官方值，勿随意改动——双端对拍基准）
    public static readonly double[] W =
    [
        0.4872, 1.4003, 3.7145, 13.8206, 5.1618, 1.2298, 0.8975, 0.031,
        1.6474, 0.1367, 1.0461, 2.1072, 0.0793, 0.3246, 1.587, 0.2272, 2.8755,
    ];

    private const double Decay = -0.5;
    private const double Factor = 19.0 / 81.0;
    // 目标留存率：0.90 下间隔恰等于 S
    private const double DesiredRetention = 0.90;

    /// <summary>quality 0~5 → FSRS rating 1=Again 2=Hard 3=Good 4=Easy（固件同映射）</summary>
    public static int MapRating(int quality) => quality switch
    {
        <= 1 => 1,
        2 => 2,
        >= 5 => 4,
        _ => 3,
    };

    /// <summary>可提取性 R(t,S)</summary>
    public static double Retrievability(double stability, double elapsedDays) =>
        Math.Pow(1.0 + Factor * elapsedDays / stability, Decay);

    /// <summary>目标留存率下的复习间隔（天）</summary>
    public static int NextInterval(double stability) =>
        Math.Max(1, (int)Math.Round(stability * (Math.Pow(DesiredRetention, 1.0 / Decay) - 1.0) / Factor));

    /// <summary>
    /// 应用一次复习（影子模式）：只写 Fsrs* 三列，不动 SM-2 主列。
    /// 首评（FsrsStability≤0）按 S0/D0 初始化；elapsed 取 rec.LastStudiedAt，
    /// 因此须在 SrsService.ApplyReview 覆写 LastStudiedAt 之前调用。
    /// </summary>
    public void ApplyShadow(LearningRecord rec, int quality, DateTime now)
    {
        var (s, d, interval) = NextState(rec.FsrsStability, rec.FsrsDifficulty,
            rec.LastStudiedAt, quality, now);
        rec.FsrsStability = s;
        rec.FsrsDifficulty = d;
        rec.FsrsNextReview = now.AddDays(interval);
    }

    /// <summary>
    /// 应用一次复习（生效模式，M4 切换后使用）：镜像 SM-2 的主列语义
    /// （NextReview/SrsLevel/连错规则与 SrsService 严格同步），并同步影子列。
    /// </summary>
    public void ApplyReview(LearningRecord rec, int quality, DateTime now)
    {
        var (s, d, interval) = NextState(rec.FsrsStability, rec.FsrsDifficulty,
            rec.LastStudiedAt, quality, now);

        rec.FsrsStability = s;
        rec.FsrsDifficulty = d;
        rec.FsrsNextReview = now.AddDays(interval);

        // 主列（调用方零改动：DailyPushJob/看板继续读 NextReview/SrsLevel）
        rec.IntervalDays = interval;
        rec.NextReview = rec.FsrsNextReview.Value;
        rec.ReviewCount++;
        rec.SrsLevel = quality >= 3
            ? Math.Min(5, rec.SrsLevel + 1)
            : 0;
        // 连错规则与 SM-2/固件 learning_state 同步：q<3 递增（错词本），≥3 清零
        rec.ConsecutiveWrong = quality < 3 ? rec.ConsecutiveWrong + 1 : 0;
        rec.LastQuality = quality;
        rec.LastStudiedAt = now;
    }

    /// <summary>初始化新记录（生效模式主列 + 影子列归零）</summary>
    public void InitRecord(LearningRecord rec, DateTime now)
    {
        rec.FsrsStability = 0;
        rec.FsrsDifficulty = 0;
        rec.FsrsNextReview = null;
        rec.LastQuality = 0;
        rec.ReviewCount = 0;
        rec.SrsLevel = 0;
        rec.IntervalDays = 0;
        rec.NextReview = now;
        rec.LastStudiedAt = now; // 首评走 S≤0 分支不依赖 lastStudied；避免 DateTime.MinValue 写 PG 越界
        rec.ConsecutiveWrong = 0;
    }

    /// <summary>单步状态推进（纯函数，双端对拍核心）</summary>
    private static (double S, double D, int Interval) NextState(
        double s, double d, DateTime lastStudied, int quality, DateTime now)
    {
        var r = MapRating(quality);

        if (s <= 0)
        {
            // 首评：S0/D0 初始化
            s = W[r - 1];
            d = Math.Clamp(W[4] - Math.Exp(W[5] * (r - 1)), 1.0, 10.0);
        }
        else
        {
            var elapsed = Math.Max(0.0, (now - lastStudied).TotalDays);
            var retrievability = Math.Clamp(Retrievability(s, elapsed), 0.005, 0.999);

            if (r == 1)
            {
                // 遗忘：S_f = w11·D^(-w12)·((S+1)^w13 - 1)·e^(w14·(1-R))
                s = W[11] * Math.Pow(d, -W[12])
                    * (Math.Pow(s + 1.0, W[13]) - 1.0)
                    * Math.Exp(W[14] * (1.0 - retrievability));
            }
            else
            {
                // 回忆：HP/EB 修正
                var hp = r == 2 ? W[15] : 1.0;
                var eb = r == 4 ? W[16] : 1.0;
                var inc = Math.Exp(W[8]) * (11.0 - d) * Math.Pow(s, -W[9])
                    * (Math.Exp(W[10] * (1.0 - retrievability)) - 1.0) * hp * eb;
                s *= 1.0 + inc;
            }

            s = Math.Max(0.01, s);
            d = Math.Clamp(d - W[6] * (r - 3), 1.0, 10.0);
        }

        return (s, d, NextInterval(s));
    }
}
