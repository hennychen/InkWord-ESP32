using InkWord.Core.Entities;

namespace InkWord.Services;

/// <summary>
/// SRS 间隔重复算法服务（SM-2），服务端镜像固件 srs_engine。
/// 根据回忆质量分更新 LearningRecord 的 EaseFactor / Interval / NextReview。
/// </summary>
public class SrsService
{
    private const double MinEf = 1.3;
    private const double MaxEf = 2.8;
    private const double InitEf = 2.5;

    /// <summary>
    /// 应用一次复习结果，原地更新 record。
    /// </summary>
    /// <param name="quality">回忆质量 0~5</param>
    public void ApplyReview(LearningRecord rec, int quality, DateTime now)
    {
        // 1. EaseFactor 更新
        var ef = rec.EaseFactor + (0.1 - (5 - quality) * (0.08 + (5 - quality) * 0.02));
        rec.EaseFactor = Math.Clamp(ef, MinEf, MaxEf);

        // 复习次数 + 难度等级（按 quality 区分对错）
        if (quality < 3)
        {
            // 答错：重置
            rec.IntervalDays = 1;
            rec.SrsLevel = 0;
        }
        else
        {
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

    /// <summary>初始化一条新学习记录的默认 SRS 状态。</summary>
    public void InitRecord(LearningRecord rec, DateTime now)
    {
        rec.EaseFactor = InitEf;
        rec.ReviewCount = 0;
        rec.IntervalDays = 0;
        rec.SrsLevel = 0;
        rec.NextReview = now;
        rec.LastStudiedAt = now;
    }
}
