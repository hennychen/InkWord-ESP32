using InkWord.Core.Common;

namespace InkWord.Core.Entities;

/// <summary>
/// 单个单词的学习记录（SRS 状态）。
/// </summary>
public class LearningRecord : BaseEntity
{
    public Guid DeviceId { get; set; }
    public Device? Device { get; set; }

    public Guid WordId { get; set; }
    public Word? Word { get; set; }

    /// <summary>最近一次回忆质量分 0~5</summary>
    public int LastQuality { get; set; }

    /// <summary>复习次数</summary>
    public int ReviewCount { get; set; }

    /// <summary>SRS 等级 0~5（用于统计分布）</summary>
    public int SrsLevel { get; set; }

    /// <summary>难度系数</summary>
    public double EaseFactor { get; set; } = 2.5;

    /// <summary>当前间隔（天）</summary>
    public int IntervalDays { get; set; }

    /// <summary>下次复习时间</summary>
    public DateTime NextReview { get; set; }

    /// <summary>最后学习时间</summary>
    public DateTime LastStudiedAt { get; set; }

    /// <summary>连续答错次数（错词本：&gt;0 即在错词本中，答对归零；
    /// 维护规则与固件 learning_state 同步：quality&lt;3 递增，≥3 清零）</summary>
    public int ConsecutiveWrong { get; set; }

    /// <summary>是否收藏（设备端 SET 长按切换，/sync/collect 上报）</summary>
    public bool IsCollected { get; set; }

    // ---- FSRS 影子列（M3 路径 A，2026-08-22）----
    // 影子运行阶段仅积累不生效（SM-2 仍为主调度）；M4 切换后由
    // FsrsService 同步写主列（NextReview 等），影子列停写保留历史。

    /// <summary>FSRS 记忆稳定性 S（天）；0 = 未启用影子</summary>
    public double FsrsStability { get; set; }

    /// <summary>FSRS 难度 D ∈ [1,10]</summary>
    public double FsrsDifficulty { get; set; }

    /// <summary>FSRS 影子排期的下次复习时间</summary>
    public DateTime? FsrsNextReview { get; set; }

    /// <summary>最近一次发音评测得分 0~100（M5 路径 C，无记录为 null）</summary>
    public int? LastPronScore { get; set; }
}
