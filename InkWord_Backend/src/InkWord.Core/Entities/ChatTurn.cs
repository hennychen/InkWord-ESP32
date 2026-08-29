namespace InkWord.Core.Entities;

/// <summary>
/// 对话轮落库（A3，2026-08-29）：ChatService 每轮异步写入（Hangfire
/// fire-and-forget，对话端点响应不等待，失败仅日志），周报复盘与
/// 生词联动的数据源。
///
/// append-only 时序日志：不继承 BaseEntity（软删除/更新时间对日志无
/// 语义，列集最小化高写入场景）；纯 Id 关联 Device 不建 FK（T4.1
/// 纪律）；90 天前记录由 ChatTurnCleanupJob 夜间回收（对齐心跳类
/// 数据保留粒度）。
/// </summary>
public class ChatTurn
{
    public Guid Id { get; set; } = Guid.NewGuid();

    public Guid DeviceId { get; set; }

    /// <summary>对话模式：free/scenario/translate（ChatService 归一化后值）</summary>
    public string Mode { get; set; } = "free";

    /// <summary>场景短码（仅 scenario 模式非空，如 "food"）</summary>
    public string? ScenarioId { get; set; }

    /// <summary>学生原句（ASR 转写）</summary>
    public string Transcript { get; set; } = "";

    /// <summary>外教/教练回复（护栏截断后）</summary>
    public string Reply { get; set; } = "";

    /// <summary>轮次时间（UTC）</summary>
    public DateTime Ts { get; set; } = DateTime.UtcNow;
}
