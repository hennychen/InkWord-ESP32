using InkWord.Core.Common;

namespace InkWord.Core.Entities;

/// <summary>
/// 卡组（v1.4 T4.1 全科地基）：科目内的具体词书/卡组，
/// 与固件 SD manifest 的 deck 条目对应（Code ↔ id，≤7 字符约束同源）。
/// </summary>
public class Deck : BaseEntity
{
    /// <summary>归属账户（v1.5 T5.3 轻账户；null = 官方/公共卡组）。
    /// 只管内容归属与云备份，设备侧零感知（ACCOUNT_MODEL_DECISION §四）</summary>
    public Guid? OwnerId { get; set; }

    /// <summary>归属科目</summary>
    public Guid SubjectId { get; set; }
    public Subject? Subject { get; set; }

    /// <summary>卡组短码（科目内唯一；固件 deck_active / NVS 后缀预算 ≤7 字符）</summary>
    public string Code { get; set; } = string.Empty;

    /// <summary>显示名（设备菜单 / App 词书页）</summary>
    public string Name { get; set; } = string.Empty;

    /// <summary>默认卡面类型：word-card / qa-card / poem-card（T4.3 分派契约）</summary>
    public string PayloadType { get; set; } = "word-card";

    /// <summary>简介（App 词书页副标题）</summary>
    public string Description { get; set; } = string.Empty;

    // 导航：仅 Deck→Subject 单向（Word 侧纯 Id 关联 —— 三表 Item
    // 混合模型不建 Words 集合，关联查询显式 join，见 T4.1）
}
