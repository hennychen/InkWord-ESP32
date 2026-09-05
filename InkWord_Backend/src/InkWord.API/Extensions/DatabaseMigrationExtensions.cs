using InkWord.Core.Entities;
using InkWord.Infrastructure.DbContext;
using Microsoft.EntityFrameworkCore;
using Serilog;

namespace InkWord.API.Extensions;

/// <summary>
/// 数据库迁移扩展（从 Program.cs 提取，2026-09-05）。
/// 开发环境自动建表 + 幂等补列；正式迁移机制（EF Migrations）
/// 引入后删除本文件所有补丁。
/// </summary>
public static class DatabaseMigrationExtensions
{
    /// <summary>确保数据库已创建并应用所有幂等补丁</summary>
    public static void MigrateInkWordDatabase(this IApplicationBuilder app)
    {
        using var scope = app.ApplicationServices.CreateScope();
        var db = scope.ServiceProvider.GetRequiredService<AppDbContext>();
        db.Database.EnsureCreated();

        ApplyV21Patch(db);          // V2.1 词库扩展
        ApplyFsrsPatch(db);         // FSRS 影子列 + 发音评分
        ApplyT41Patch(db);          // 全科地基 Subjects/Decks
        ApplyV15T53Patch(db);       // 轻账户 Accounts
        ApplyA3Patch(db);           // 对话复盘 ChatTurns/ChatReviews
        ApplyV20Patch(db);          // 完整账户 Device.UserId
        ApplyV21SharePatch(db);     // UGC 分享 Decks.IsShared
        ApplyReaderPatch(db);       // 阅读器后端 Books/ReadingProgress/DeviceBookmarks
        SeedAdminUser(db);          // 管理端默认账号
    }

    /// <summary>V2.1 词库扩展：Words 四字段</summary>
    private static void ApplyV21Patch(AppDbContext db)
    {
        var sqls = new[]
        {
            "ALTER TABLE \"Words\" ADD COLUMN IF NOT EXISTS \"Root\" text NOT NULL DEFAULT ''",
            "ALTER TABLE \"Words\" ADD COLUMN IF NOT EXISTS \"Inflections\" text NOT NULL DEFAULT ''",
            "ALTER TABLE \"Words\" ADD COLUMN IF NOT EXISTS \"Source\" text NOT NULL DEFAULT ''",
            "ALTER TABLE \"Words\" ADD COLUMN IF NOT EXISTS \"Grade\" text NOT NULL DEFAULT ''",
            "ALTER TABLE \"Words\" ADD COLUMN IF NOT EXISTS \"AiStatus\" integer NOT NULL DEFAULT 0",
            "ALTER TABLE \"Words\" ADD COLUMN IF NOT EXISTS \"AiSuggestion\" text",
        };
        foreach (var sql in sqls) db.Database.ExecuteSqlRaw(sql);
    }

    /// <summary>FSRS 影子列 + 发音评分</summary>
    private static void ApplyFsrsPatch(AppDbContext db)
    {
        var sqls = new[]
        {
            "ALTER TABLE \"LearningRecords\" ADD COLUMN IF NOT EXISTS \"FsrsStability\" double precision NOT NULL DEFAULT 0",
            "ALTER TABLE \"LearningRecords\" ADD COLUMN IF NOT EXISTS \"FsrsDifficulty\" double precision NOT NULL DEFAULT 0",
            "ALTER TABLE \"LearningRecords\" ADD COLUMN IF NOT EXISTS \"FsrsNextReview\" timestamptz",
            "ALTER TABLE \"LearningRecords\" ADD COLUMN IF NOT EXISTS \"LastPronScore\" integer",
        };
        foreach (var sql in sqls) db.Database.ExecuteSqlRaw(sql);
    }

    /// <summary>T4.1 全科地基：Subjects/Decks 建表 + 种子 + 存量回填</summary>
    private static void ApplyT41Patch(AppDbContext db)
    {
        var sqls = new[]
        {
            // Words 六列（Item 混合模型）
            "ALTER TABLE \"Words\" ADD COLUMN IF NOT EXISTS \"SubjectId\" uuid",
            "ALTER TABLE \"Words\" ADD COLUMN IF NOT EXISTS \"DeckId\" uuid",
            "ALTER TABLE \"Words\" ADD COLUMN IF NOT EXISTS \"Front\" text NOT NULL DEFAULT ''",
            "ALTER TABLE \"Words\" ADD COLUMN IF NOT EXISTS \"Back\" text NOT NULL DEFAULT ''",
            "ALTER TABLE \"Words\" ADD COLUMN IF NOT EXISTS \"PayloadJson\" text",
            // 墨封标记
            "ALTER TABLE \"LearningRecords\" ADD COLUMN IF NOT EXISTS \"IsMastered\" boolean NOT NULL DEFAULT false",

            // Subjects/Decks 建表
            @"CREATE TABLE IF NOT EXISTS ""Subjects"" (
                ""Id"" uuid NOT NULL PRIMARY KEY,
                ""Code"" varchar(16) NOT NULL,
                ""Name"" varchar(64) NOT NULL,
                ""SortOrder"" integer NOT NULL,
                ""CreatedAt"" timestamp with time zone NOT NULL,
                ""UpdatedAt"" timestamp with time zone,
                ""IsDeleted"" boolean NOT NULL)",
            @"CREATE UNIQUE INDEX IF NOT EXISTS ""IX_Subjects_Code"" ON ""Subjects"" (""Code"")",
            @"CREATE TABLE IF NOT EXISTS ""Decks"" (
                ""Id"" uuid NOT NULL PRIMARY KEY,
                ""SubjectId"" uuid NOT NULL,
                ""Code"" varchar(16) NOT NULL,
                ""Name"" varchar(128) NOT NULL,
                ""PayloadType"" varchar(16) NOT NULL,
                ""Description"" varchar(512) NOT NULL,
                ""CreatedAt"" timestamp with time zone NOT NULL,
                ""UpdatedAt"" timestamp with time zone,
                ""IsDeleted"" boolean NOT NULL)",
            @"CREATE UNIQUE INDEX IF NOT EXISTS ""IX_Decks_SubjectId_Code"" ON ""Decks"" (""SubjectId"", ""Code"")",
            @"CREATE INDEX IF NOT EXISTS ""IX_Words_SubjectId"" ON ""Words"" (""SubjectId"")",
            @"CREATE INDEX IF NOT EXISTS ""IX_Words_DeckId"" ON ""Words"" (""DeckId"")",
            // 种子
            @"INSERT INTO ""Subjects"" (""Id"",""Code"",""Name"",""SortOrder"",""CreatedAt"",""IsDeleted"")
               VALUES ('ee000000-0000-0000-0000-000000000001','en','英语',0,now(),false)
               ON CONFLICT (""Code"") DO NOTHING",
            @"INSERT INTO ""Decks"" (""Id"",""SubjectId"",""Code"",""Name"",""PayloadType"",""Description"",""CreatedAt"",""IsDeleted"")
               VALUES ('ee000000-0000-0000-0000-0000000000d1',
                       'ee000000-0000-0000-0000-000000000001','junior','初中英语（默认）','word-card',
                       '存量英语词条整体迁移归属（T4.1）',now(),false)
               ON CONFLICT (""SubjectId"",""Code"") DO NOTHING",
            // 存量回填
            @"UPDATE ""Words"" SET ""SubjectId""='ee000000-0000-0000-0000-000000000001' WHERE ""SubjectId"" IS NULL",
            @"UPDATE ""Words"" SET ""DeckId""='ee000000-0000-0000-0000-0000000000d1' WHERE ""DeckId"" IS NULL",
            @"UPDATE ""Words"" SET ""Front""=""Text"" WHERE ""Front""='' AND ""Text""<>''",
            @"UPDATE ""Words"" SET ""Back""=""Meaning"" WHERE ""Back""='' AND ""Meaning""<>''",
        };
        foreach (var sql in sqls) db.Database.ExecuteSqlRaw(sql);
    }

    /// <summary>v1.5 T5.3 轻账户：Accounts 建表 + Decks.OwnerId</summary>
    private static void ApplyV15T53Patch(AppDbContext db)
    {
        var sqls = new[]
        {
            @"CREATE TABLE IF NOT EXISTS ""Accounts"" (
                ""Id"" uuid NOT NULL PRIMARY KEY,
                ""Username"" varchar(64) NOT NULL,
                ""PasswordHash"" varchar(256) NOT NULL,
                ""DisplayName"" varchar(64) NOT NULL,
                ""CreatedAt"" timestamp with time zone NOT NULL,
                ""UpdatedAt"" timestamp with time zone,
                ""IsDeleted"" boolean NOT NULL)",
            @"CREATE UNIQUE INDEX IF NOT EXISTS ""IX_Accounts_Username"" ON ""Accounts"" (""Username"")",
            @"ALTER TABLE ""Decks"" ADD COLUMN IF NOT EXISTS ""OwnerId"" uuid",
            @"CREATE INDEX IF NOT EXISTS ""IX_Decks_OwnerId"" ON ""Decks"" (""OwnerId"")",
        };
        foreach (var sql in sqls) db.Database.ExecuteSqlRaw(sql);
    }

    /// <summary>A3 对话复盘：ChatTurns + ChatReviews</summary>
    private static void ApplyA3Patch(AppDbContext db)
    {
        var sqls = new[]
        {
            @"CREATE TABLE IF NOT EXISTS ""ChatTurns"" (
                ""Id"" uuid NOT NULL PRIMARY KEY,
                ""DeviceId"" uuid NOT NULL,
                ""Mode"" varchar(16) NOT NULL,
                ""ScenarioId"" varchar(16),
                ""Transcript"" varchar(512) NOT NULL,
                ""Reply"" varchar(1024) NOT NULL,
                ""Ts"" timestamp with time zone NOT NULL)",
            @"CREATE INDEX IF NOT EXISTS ""IX_ChatTurns_DeviceId_Ts"" ON ""ChatTurns"" (""DeviceId"", ""Ts"")",
            @"CREATE TABLE IF NOT EXISTS ""ChatReviews"" (
                ""Id"" uuid NOT NULL PRIMARY KEY,
                ""DeviceId"" uuid NOT NULL,
                ""WeekStart"" timestamp with time zone NOT NULL,
                ""PayloadJson"" text NOT NULL,
                ""TurnCount"" integer NOT NULL,
                ""CreatedAt"" timestamp with time zone NOT NULL)",
            @"CREATE UNIQUE INDEX IF NOT EXISTS ""IX_ChatReviews_DeviceId_WeekStart"" ON ""ChatReviews"" (""DeviceId"", ""WeekStart"")",
        };
        foreach (var sql in sqls) db.Database.ExecuteSqlRaw(sql);
    }

    /// <summary>v2.0 完整账户：Device.UserId 兑现</summary>
    private static void ApplyV20Patch(AppDbContext db)
    {
        var sqls = new[]
        {
            "ALTER TABLE \"Devices\" ADD COLUMN IF NOT EXISTS \"UserId\" uuid",
            @"DO $$ DECLARE r record; BEGIN
                FOR r IN SELECT conname FROM pg_constraint c
                         JOIN pg_class d ON d.oid = c.conrelid
                         JOIN pg_class u ON u.oid = c.confrelid
                         WHERE c.contype = 'f' AND d.relname = 'Devices' AND u.relname = 'Users'
                LOOP EXECUTE 'ALTER TABLE ""Devices"" DROP CONSTRAINT ' || quote_ident(r.conname);
                END LOOP; END $$;",
            "CREATE INDEX IF NOT EXISTS \"IX_Devices_UserId\" ON \"Devices\" (\"UserId\")",
        };
        foreach (var sql in sqls) db.Database.ExecuteSqlRaw(sql);
    }

    /// <summary>v2.0 UGC 分享：Decks 分享开关 + 部分索引</summary>
    private static void ApplyV21SharePatch(AppDbContext db)
    {
        var sqls = new[]
        {
            "ALTER TABLE \"Decks\" ADD COLUMN IF NOT EXISTS \"IsShared\" boolean NOT NULL DEFAULT false",
            "ALTER TABLE \"Decks\" ADD COLUMN IF NOT EXISTS \"SharedAt\" timestamp with time zone",
            "CREATE INDEX IF NOT EXISTS \"IX_Decks_IsShared\" ON \"Decks\" (\"IsShared\") WHERE \"IsShared\"",
        };
        foreach (var sql in sqls) db.Database.ExecuteSqlRaw(sql);
    }

    /// <summary>阅读器后端：Books / ReadingProgress / DeviceBookmarks</summary>
    private static void ApplyReaderPatch(AppDbContext db)
    {
        var sqls = new[]
        {
            @"CREATE TABLE IF NOT EXISTS ""Books"" (
                ""Id"" uuid NOT NULL PRIMARY KEY,
                ""BookKey"" varchar(64) NOT NULL,
                ""Title"" varchar(128) NOT NULL,
                ""Author"" varchar(64) NOT NULL DEFAULT '',
                ""Language"" varchar(8) NOT NULL DEFAULT 'zh',
                ""Tags"" varchar(256) NOT NULL DEFAULT '',
                ""FileSize"" bigint NOT NULL DEFAULT 0,
                ""Format"" varchar(8) NOT NULL DEFAULT 'txt',
                ""CoverUrl"" varchar(512) NOT NULL DEFAULT '',
                ""Description"" varchar(1024) NOT NULL DEFAULT '',
                ""Published"" boolean NOT NULL DEFAULT false,
                ""DownloadCount"" integer NOT NULL DEFAULT 0,
                ""CreatedAt"" timestamp with time zone NOT NULL,
                ""UpdatedAt"" timestamp with time zone,
                ""IsDeleted"" boolean NOT NULL)",
            @"CREATE UNIQUE INDEX IF NOT EXISTS ""IX_Books_BookKey"" ON ""Books"" (""BookKey"")",

            @"CREATE TABLE IF NOT EXISTS ""ReadingProgresses"" (
                ""Id"" uuid NOT NULL PRIMARY KEY,
                ""DeviceId"" uuid NOT NULL,
                ""BookId"" uuid NOT NULL,
                ""CurrentPage"" integer NOT NULL DEFAULT 0,
                ""TotalPages"" integer NOT NULL DEFAULT 0,
                ""FontLevel"" integer NOT NULL DEFAULT 1,
                ""Signature"" bigint NOT NULL DEFAULT 0,
                ""LastReadAt"" timestamp with time zone NOT NULL,
                ""TotalReadMinutes"" integer NOT NULL DEFAULT 0,
                ""CreatedAt"" timestamp with time zone NOT NULL,
                ""UpdatedAt"" timestamp with time zone,
                ""IsDeleted"" boolean NOT NULL)",
            @"CREATE UNIQUE INDEX IF NOT EXISTS ""IX_ReadingProgresses_DeviceId_BookId"" ON ""ReadingProgresses"" (""DeviceId"", ""BookId"")",
            @"CREATE INDEX IF NOT EXISTS ""IX_ReadingProgresses_LastReadAt"" ON ""ReadingProgresses"" (""LastReadAt"")",

            @"CREATE TABLE IF NOT EXISTS ""DeviceBookmarks"" (
                ""Id"" uuid NOT NULL PRIMARY KEY,
                ""DeviceId"" uuid NOT NULL,
                ""BookId"" uuid NOT NULL,
                ""Page"" integer NOT NULL,
                ""ByteOffset"" bigint NOT NULL DEFAULT 0,
                ""Note"" varchar(64) NOT NULL DEFAULT '',
                ""CreatedAt"" timestamp with time zone NOT NULL,
                ""UpdatedAt"" timestamp with time zone,
                ""IsDeleted"" boolean NOT NULL)",
            @"CREATE UNIQUE INDEX IF NOT EXISTS ""IX_DeviceBookmarks_DeviceId_BookId_Page"" ON ""DeviceBookmarks"" (""DeviceId"", ""BookId"", ""Page"")",
        };
        foreach (var sql in sqls) db.Database.ExecuteSqlRaw(sql);
    }

    /// <summary>管理端默认账号（admin/admin123，仅首次启动）</summary>
    private static void SeedAdminUser(AppDbContext db)
    {
        if (!db.Users.Any())
        {
            db.Users.Add(new User
            {
                Username = "admin",
                PasswordHash = InkWord.API.Controllers.AuthController.HashPassword("admin123"),
                DisplayName = "Administrator",
                Role = "Admin",
            });
            db.SaveChangesAsync().GetAwaiter().GetResult();
        }
    }
}
