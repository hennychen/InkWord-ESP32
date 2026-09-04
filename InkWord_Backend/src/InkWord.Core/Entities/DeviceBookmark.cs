using InkWord.Core.Common;

namespace InkWord.Core.Entities;

/// <summary>
/// 设备书签云端备份（每设备每书每页一条）。
/// 设备端 NVS 是权威源，云端做灾备和统计。
/// </summary>
public class DeviceBookmark : BaseEntity
{
    /// <summary>设备 ID</summary>
    public Guid DeviceId { get; set; }

    /// <summary>书籍 ID</summary>
    public Guid BookId { get; set; }

    /// <summary>书签页码</summary>
    public int Page { get; set; }

    /// <summary>字节偏移（精确定位）</summary>
    public uint ByteOffset { get; set; }

    /// <summary>备注（空串=无备注，最长 32 字符）</summary>
    public string Note { get; set; } = string.Empty;

    // 导航
    public Device? Device { get; set; }
    public Book? Book { get; set; }
}
