using InkWord.Core.Common;

namespace InkWord.Core.Entities;

/// <summary>
/// 设备阅读进度（每设备每书一条，设备推送云端备份）。
/// </summary>
public class ReadingProgress : BaseEntity
{
    /// <summary>设备 ID</summary>
    public Guid DeviceId { get; set; }

    /// <summary>书籍 ID</summary>
    public Guid BookId { get; set; }

    /// <summary>当前页码</summary>
    public int CurrentPage { get; set; }

    /// <summary>总页数（设备上报，用于计算百分比）</summary>
    public int TotalPages { get; set; }

    /// <summary>字号级 0/1/2</summary>
    public int FontLevel { get; set; } = 1;

    /// <summary>内容签名（FNV-1a，书变更检测）</summary>
    public uint Signature { get; set; }

    /// <summary>最后阅读时间</summary>
    public DateTime LastReadAt { get; set; }

    /// <summary>累计阅读时长（分钟，设备上报）</summary>
    public int TotalReadMinutes { get; set; }

    // 导航（纯 Id 关联不建 FK，与 LearningRecord→Device 同先例）
    public Device? Device { get; set; }
    public Book? Book { get; set; }
}
