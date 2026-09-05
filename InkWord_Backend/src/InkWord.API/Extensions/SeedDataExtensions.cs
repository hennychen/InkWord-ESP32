using InkWord.Infrastructure.DbContext;
using Serilog;

namespace InkWord.API.Extensions;

/// <summary>
/// 种子数据编排扩展（从 Program.cs 提取，2026-09-05）。
/// </summary>
public static class SeedDataExtensions
{
    /// <summary>导入系统默认词库 + 古诗文 Deck</summary>
    public static async Task SeedInkWordDataAsync(this IApplicationBuilder app)
    {
        // 默认词库（Words 表为空时导入 default_words.csv）
        using (var scope = app.ApplicationServices.CreateScope())
        {
            try
            {
                var db = scope.ServiceProvider.GetRequiredService<AppDbContext>();
                var seeded = await DefaultWordSeeder.SeedAsync(db);
                if (seeded > 0)
                    Log.Information("默认词库已导入 {Count} 条", seeded);
            }
            catch (Exception ex)
            {
                Log.Warning(ex, "默认词库种子跳过（表未就绪或文件缺失）");
            }
        }

        // 古诗文 Deck（Decks 无 poems 时导入）
        using (var scope = app.ApplicationServices.CreateScope())
        {
            try
            {
                var db = scope.ServiceProvider.GetRequiredService<AppDbContext>();
                var poems = await PoemSeeder.SeedAsync(db);
                if (poems > 0)
                    Log.Information("古诗 Deck 已导入 {Count} 条", poems);
            }
            catch (Exception ex)
            {
                Log.Warning(ex, "古诗 Deck 种子跳过（表未就绪或文件缺失）");
            }
        }
    }
}
