namespace InkWord.Core.Common;

/// <summary>
/// 所有实体的抽象基类，统一主键与审计字段。
/// </summary>
public abstract class BaseEntity
{
    public Guid Id { get; set; } = Guid.NewGuid();
    public DateTime CreatedAt { get; set; } = DateTime.UtcNow;
    public DateTime? UpdatedAt { get; set; }
    public bool IsDeleted { get; set; }   // 软删除标记
}
