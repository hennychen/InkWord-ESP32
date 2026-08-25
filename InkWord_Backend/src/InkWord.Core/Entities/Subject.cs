using InkWord.Core.Common;

namespace InkWord.Core.Entities;

/// <summary>
/// 科目（v1.4 T4.1 全科地基）：英语 en / 语文 zh 等。
/// Code 进导出协议 subject 字段与固件 manifest 预留字段（v1.3 已解析即忽略）。
/// </summary>
public class Subject : BaseEntity
{
    /// <summary>科目代码（唯一，小写短码：en/zh/…）</summary>
    public string Code { get; set; } = "en";

    /// <summary>显示名（英语/语文）</summary>
    public string Name { get; set; } = string.Empty;

    /// <summary>科目排序（菜单展示序：语文古诗文为第二科目见 T4.4）</summary>
    public int SortOrder { get; set; }

    // 导航
    public ICollection<Deck> Decks { get; set; } = new List<Deck>();
}
