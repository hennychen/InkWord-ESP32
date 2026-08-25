using System.Text;
using InkWord.Core.Entities;
using InkWord.Infrastructure.DbContext;
using Microsoft.EntityFrameworkCore;

namespace InkWord.API;

/// <summary>
/// 语文古诗文 Deck 种子（v1.4 T4.4）：SeedData/poems_cb.csv 部编版
/// 必背名联，拆上下句默写条目。幂等判据 = Decks 含 poems（不同于
/// DefaultWordSeeder 的空表判据——英语默认库已存在时仍可补古诗）。
/// 字段映射（T4.3 渲染契约 + T4.4 默写契约，设备零 payloadJson 解析）：
///   Text = 下句（默写答案，词卡 text 位）/ Root = 上句（默写题面，
///   poem 学习视图随译文入正文流）/ Phonetic = 下句拼音 /
///   Meaning = 译文 / Tag = 诗题（诗人，固件 32B 预算内简称）/
///   Source·Grade = 部编版学段；PayloadJson = {"prev":上句} 结构化
///   留云端 / App 二期编辑器消费。注音与译文后续可经 AI 管线批量
/// 重生成（T3.3 subject=zh prompt 已参数化，本种子为开箱基线）。
/// </summary>
public static class PoemSeeder
{
    /// <summary>语文科目固定 Guid（t41Sql en 种子同族，ee…0002）</summary>
    public static readonly Guid ZhSubjectId =
        new("ee000000-0000-0000-0000-000000000002");

    /// <summary>古诗卡组固定 Guid（junior 种子 ee…00d1 同族）。
    /// Code=poems 5 字符 ≤ 固件 NVS 后缀预算 7（lr_st_poems=11B）。 </summary>
    public static readonly Guid PoemsDeckId =
        new("ee000000-0000-0000-0000-0000000000d2");

    /// <summary>CSV 行 → Word 条目（纯函数，测试直用）。
    /// 列序对齐 AdminWordController.ImportCsv / default_words.csv；
    /// 数据端保证字段内无英文逗号（Split(',') 简单格式）。 </summary>
    public static List<Word> ParsePoemsCsv(string content)
    {
        var words = new List<Word>();
        foreach (var line in content.Split('\n',
                     StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            if (line.StartsWith("text,")) continue;   // 表头
            var f = line.Split(',');
            if (f.Length < 3) continue;

            var prev = f.Length > 7 ? f[7] : "";
            words.Add(new Word
            {
                Text = f[0],
                Phonetic = f.Length > 1 ? f[1] : "",
                Meaning = f.Length > 2 ? f[2] : "",
                Tag = f.Length > 5 ? f[5] : "",
                Difficulty = f.Length > 6 && int.TryParse(f[6], out var d) ? d : 1,
                Root = prev,
                Source = f.Length > 9 ? f[9] : "",
                Grade = f.Length > 10 ? f[10] : "",
                SubjectId = ZhSubjectId,
                DeckId = PoemsDeckId,
                Front = f[0],                          // v2 卡面镜像（T4.1）
                Back = f.Length > 2 ? f[2] : "",
                // 数据端无引号/反斜杠（古诗文本），直拼安全
                PayloadJson = prev != "" ? $"{{\"prev\":\"{prev}\"}}" : "",
            });
        }
        return words;
    }

    /// <summary>Decks 无 poems 时导入 zh 科目 + poems 卡组 + 古诗条目
    /// （幂等：卡组在即跳过）。Version 接 Words 最大值续增——
    /// GetIncrementalAsync 按 Version 增量下发，古诗随英语库之后同步。</summary>
    public static async Task<int> SeedAsync(
        AppDbContext db, CancellationToken ct = default)
    {
        if (await db.Decks.AnyAsync(d => d.Code == "poems", ct)) return 0;

        var path = Path.Combine(
            AppContext.BaseDirectory, "SeedData", "poems_cb.csv");
        if (!File.Exists(path))
            throw new FileNotFoundException("poems_cb.csv missing", path);

        var words = ParsePoemsCsv(
            await File.ReadAllTextAsync(path, Encoding.UTF8, ct));
        if (words.Count == 0) return 0;

        // zh 科目可能已存在（将来其他语文卡组先建）：先查后建防
        // IX_Subjects_Code 冲突（t41Sql ON CONFLICT 同语义）
        if (!await db.Subjects.AnyAsync(s => s.Code == "zh", ct))
            db.Subjects.Add(new Subject
            {
                Id = ZhSubjectId,
                Code = "zh",
                Name = "语文",
                SortOrder = 1,
            });
        db.Decks.Add(new Deck
        {
            Id = PoemsDeckId,
            SubjectId = ZhSubjectId,
            Code = "poems",
            Name = "部编版必背古诗",
            PayloadType = "poem-card",
            Description = "部编版必背名联上下句默写（T4.4 第二科目）",
        });

        var version = await db.Words.MaxAsync(w => (int?)w.Version, ct) ?? 0;
        foreach (var w in words)
        {
            w.Version = ++version;
            w.ChangeType = 0;
        }
        db.Words.AddRange(words);
        await db.SaveChangesAsync(ct);
        return words.Count;
    }
}
