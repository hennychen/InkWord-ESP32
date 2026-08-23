using InkWord.Core.Entities;

namespace InkWord.Services;

/// <summary>
/// SRS 间隔重复算法服务（SM-2），服务端镜像固件 srs_engine。
/// 根据回忆质量分更新 LearningRecord 的 EaseFactor / Interval / NextReview。
///
/// 引擎开关（M3/M4 路径 A，2026-08-22）：配置 Srs:Engine = "sm2"（默认，
/// 影子运行 FSRS）| "fsrs"（FSRS 生效，SM-2 主列停写）。调用方签名不变，
/// 回滚 = 配置切回 sm2。
/// </summary>
public class SrsService
{
    private const double MinEf = 1.3;
    private const double MaxEf = 2.8;
    private const double InitEf = 2.5;

    private readonly FsrsService _fsrs;
    private readonly bool _useFsrs;

    public SrsService(FsrsService fsrs, Microsoft.Extensions.Configuration.IConfiguration cfg)
    {
        _fsrs = fsrs;
        _useFsrs = string.Equals(cfg["Srs:Engine"], "fsrs", StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>
    /// 应用一次复习结果，原地更新 record。
    /// </summary>
    /// <param name="quality">回忆质量 0~5</param>
    public void ApplyReview(LearningRecord rec, int quality, DateTime now)
    {
        if (_useFsrs)
        {
            _fsrs.ApplyReview(rec, quality, now);
            return;
        }

        // 影子 FSRS：须在 SM-2 覆写 LastStudiedAt 之前调用（elapsed 依赖旧值）
        _fsrs.ApplyShadow(rec, quality, now);

        // 1. EaseFactor 更新
        var ef = rec.EaseFactor + (0.1 - (5 - quality) * (0.08 + (5 - quality) * 0.02));
        rec.EaseFactor = Math.Clamp(ef, MinEf, MaxEf);

        // 复习次数 + 难度等级（按 quality 区分对错）
        if (quality < 3)
        {
            // 答错：重置 + 连错递增（错词本依据，与固件 learning_state 同步规则）
            rec.ConsecutiveWrong++;
            rec.IntervalDays = 1;
            rec.SrsLevel = 0;
        }
        else
        {
            // 答对：连错清零（移出错词本）
            rec.ConsecutiveWrong = 0;
            rec.IntervalDays = rec.ReviewCount switch
            {
                0 => 1,
                1 => 6,
                _ => (int)Math.Round(rec.IntervalDays * rec.EaseFactor)
            };
            if (rec.IntervalDays < 1) rec.IntervalDays = 1;
            rec.ReviewCount++;
            rec.SrsLevel = Math.Min(5, rec.ReviewCount);
        }

        rec.LastQuality = quality;
        rec.LastStudiedAt = now;
        rec.NextReview = now.AddDays(rec.IntervalDays);
    }

    /// <summary>初始化一条新学习记录的默认 SRS 状态（SM-2/FSRS 列均归位）。</summary>
    public void InitRecord(LearningRecord rec, DateTime now)
    {
        rec.EaseFactor = InitEf;
        rec.ReviewCount = 0;
        rec.IntervalDays = 0;
        rec.SrsLevel = 0;
        rec.NextReview = now;
        rec.LastStudiedAt = now;
        rec.ConsecutiveWrong = 0;
        rec.IsCollected = false;
        _fsrs.InitRecord(rec, now);
    }
}
