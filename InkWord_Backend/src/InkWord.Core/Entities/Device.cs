using InkWord.Core.Common;

namespace InkWord.Core.Entities;

/// <summary>
/// ESP32 墨水屏设备。
/// </summary>
public class Device : BaseEntity
{
    /// <summary>设备 MAC 地址（注册时唯一标识）</summary>
    public string MacAddress { get; set; } = string.Empty;

    /// <summary>设备名称/备注</summary>
    public string Name { get; set; } = string.Empty;

    /// <summary>设备认证密钥（32 位随机串，写入 X-Device-Key 头）</summary>
    public string ApiKey { get; set; } = string.Empty;

    /// <summary>固件版本</summary>
    public string FirmwareVersion { get; set; } = "0.0.0";

    /// <summary>本地词库版本号</summary>
    public int WordVersion { get; set; } = 0;

    /// <summary>电池电量 0~100</summary>
    public int BatteryLevel { get; set; } = 100;

    /// <summary>最后心跳时间</summary>
    public DateTime LastHeartbeat { get; set; }

    /// <summary>在线状态（由心跳推算）</summary>
    public bool IsOnline => (DateTime.UtcNow - LastHeartbeat).TotalMinutes < 5;

    // v2.0 完整账户（ADR-001 §五）：绑定学习者 Account.Id。纯 Id 关联不建
    // FK/导航（Word→Subject/Deck T4.1 同先例；原 User 导航是惯例误指后台
    // 操作员表，已删——新库 EnsureCreated 不再生成 Devices→Users FK，
    // 存量库由 Program.cs t20Sql 动态拆除）。
    public Guid? UserId { get; set; }

    public ICollection<LearningRecord> LearningRecords { get; set; } = new List<LearningRecord>();
}
