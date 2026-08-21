using InkWord.Core.Common;

namespace InkWord.Core.Entities;

/// <summary>
/// 词库词条。
/// </summary>
public class Word : BaseEntity
{
    /// <summary>单词文本（同 Tag 下唯一）</summary>
    public string Text { get; set; } = string.Empty;

    /// <summary>音标</summary>
    public string Phonetic { get; set; } = string.Empty;

    /// <summary>释义</summary>
    public string Meaning { get; set; } = string.Empty;

    /// <summary>例句</summary>
    public string Example { get; set; } = string.Empty;

    /// <summary>音频文件名（对应 OSS 对象 key）</summary>
    public string Audio { get; set; } = string.Empty;

    /// <summary>标签（年级/教材单元）</summary>
    public string Tag { get; set; } = string.Empty;

    /// <summary>词根词缀（如 "spect=看; vis=看"；V2.0 7.2 对齐，2026-08-20）</summary>
    public string Root { get; set; } = string.Empty;

    /// <summary>派生变形（逗号分隔，如 "inspect,inspection"）</summary>
    public string Inflections { get; set; } = string.Empty;

    /// <summary>教材来源（如 "人教版 九年级 Unit 5"）</summary>
    public string Source { get; set; } = string.Empty;

    /// <summary>年级（如 "九年级"；导出到设备词库底部标签行）</summary>
    public string Grade { get; set; } = string.Empty;

    /// <summary>难度 1~5</summary>
    public int Difficulty { get; set; } = 1;

    /// <summary>词库版本号（每次变更递增，用于增量同步）</summary>
    public int Version { get; set; } = 1;

    /// <summary>变更类型：0=新增 1=修改 2=删除（增量同步用）</summary>
    public int ChangeType { get; set; }

    /// <summary>是否归档（冷词）</summary>
    public bool Archived { get; set; }

    // 导航
    public ICollection<LearningRecord> LearningRecords { get; set; } = new List<LearningRecord>();
}
