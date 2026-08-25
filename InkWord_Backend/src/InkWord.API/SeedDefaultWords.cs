using System.Text;
using InkWord.Core.Entities;
using InkWord.Infrastructure.DbContext;

namespace InkWord.API;

/// <summary>
/// 系统默认词库种子（2026-08-23）。
/// 数据源为 GitHub 开源中小学词库/古诗词，经 tools/default_vocab/
/// gen_default_vocab.py 清洗生成（Split(',') 简单格式，列序对齐
/// AdminWordController.ImportCsv；生成端已保证字段内无英文逗号）。
/// </summary>
public static class DefaultWordSeeder
{
    /// <summary>Words 表为空时导入 SeedData/default_words.csv（幂等）。
    /// 仅空表导入前提下版本号本地从 1 递增，无需查库取最大版本。</summary>
    public static async Task<int> SeedAsync(
        AppDbContext db, CancellationToken ct = default)
    {
        if (db.Words.Any()) return 0;

        var path = Path.Combine(
            AppContext.BaseDirectory, "SeedData", "default_words.csv");
        if (!File.Exists(path))
            throw new FileNotFoundException("default_words.csv missing", path);

        var version = 0;
        var words = new List<Word>();
        using var reader = new StreamReader(path, Encoding.UTF8);
        await reader.ReadLineAsync(ct); // 跳过表头

        while (await reader.ReadLineAsync(ct) is { } line)
        {
            if (string.IsNullOrWhiteSpace(line)) continue;
            var f = line.Split(',');
            if (f.Length < 3) continue;

            words.Add(new Word
            {
                Text = f[0],
                Phonetic = f.Length > 1 ? f[1] : "",
                Meaning = f.Length > 2 ? f[2] : "",
                Example = f.Length > 3 ? f[3] : "",
                Audio = f.Length > 4 ? f[4] : "",
                Tag = f.Length > 5 ? f[5] : "",
                Difficulty = f.Length > 6 && int.TryParse(f[6], out var d) ? d : 1,
                Root = f.Length > 7 ? f[7] : "",
                Inflections = f.Length > 8 ? f[8] : "",
                Source = f.Length > 9 ? f[9] : "",
                Grade = f.Length > 10 ? f[10] : "",
                Version = ++version,
                ChangeType = 0,
                Front = f[0],                    // v2 卡面镜像（T4.1）
                Back = f.Length > 2 ? f[2] : "",
            });
        }

        db.Words.AddRange(words);
        await db.SaveChangesAsync(ct);
        return words.Count;
    }
}
