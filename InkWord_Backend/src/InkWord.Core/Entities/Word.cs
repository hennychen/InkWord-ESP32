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

    // ---- AI 内容增强（M1 路径 B，2026-08-22）----
    // AI 生成不直接覆盖词库字段：建议暂存 AiSuggestion，人工审核通过后
    // 回填 Example/Root 并 Version++ 走既有增量同步（设备零改动）。

    /// <summary>AI 生成状态：0=未生成 1=已生成待审 2=已应用 3=生成失败</summary>
    public int AiStatus { get; set; }

    /// <summary>AI 建议原文（JSON：kind/example/root/confusionNote；应用后清空）</summary>
    public string? AiSuggestion { get; set; }

    // ---- 全科地基（v1.4 T4.1）：Item 混合模型 ----
    // Words 表语义泛化为「词条/卡片条目」（Subject/Deck/Item 三表中的 Item）。
    // word-card：Front/Back 冗余镜像 Text/Meaning（导出 v2 双写）；
    // qa-card / poem-card（T4.3/T4.4）：Front=题面/上句，Back=答案/下句，
    // PayloadJson 携版式扩展载荷（拼音行/译文/默写上下句等）。
    // cloudId(Id)/Version/ChangeType 语义不变 —— FSRS 双端一致红线。

    /// <summary>归属科目（null = 默认英语 en，导出时兜底）</summary>
    public Guid? SubjectId { get; set; }

    /// <summary>归属卡组（null = 默认卡组 junior，导出时兜底）</summary>
    public Guid? DeckId { get; set; }

    /// <summary>卡面正面（word-card 冗存 Text；qa/poem 为题面/上句）</summary>
    public string Front { get; set; } = string.Empty;

    /// <summary>卡面背面（word-card 冗存 Meaning；qa/poem 为答案/下句）</summary>
    public string Back { get; set; } = string.Empty;

    /// <summary>版式扩展载荷 JSON（T4.3 card_layout / T4.4 默写等）</summary>
    public string? PayloadJson { get; set; }

    // 导航
    public ICollection<LearningRecord> LearningRecords { get; set; } = new List<LearningRecord>();
}
