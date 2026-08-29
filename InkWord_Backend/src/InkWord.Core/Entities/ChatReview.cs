namespace InkWord.Core.Entities;

/// <summary>
/// AI 对话周报（A3，2026-08-29）：ChatReviewJob 每周日聚合设备近 7 天
/// ChatTurn → LLM 复盘的结构化存档。
///
/// 双通道下发：Redis chatreview:{deviceId}（TTL 7 天，设备端零延迟读）
/// + 本表持久留痕（App 端历史回看）。LLM 输出为 JSON 字符串原样落
/// PayloadJson（summary/topics/highlights/suggestion/reviewWords 五段，
/// 见 ChatReviewJob prompt），设备端按需截断屏显。
/// </summary>
public class ChatReview
{
    public Guid Id { get; set; } = Guid.NewGuid();

    public Guid DeviceId { get; set; }

    /// <summary>周起始（所在周周一 00:00 UTC，同设备同周唯一）</summary>
    public DateTime WeekStart { get; set; }

    /// <summary>LLM 复盘 JSON 原文（结构见类注释）</summary>
    public string PayloadJson { get; set; } = "";

    /// <summary>聚合轮数（空周不生成记录）</summary>
    public int TurnCount { get; set; }

    public DateTime CreatedAt { get; set; } = DateTime.UtcNow;
}
