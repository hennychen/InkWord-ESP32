using InkWord.Core.Entities;
using Microsoft.EntityFrameworkCore;

namespace InkWord.Infrastructure.DbContext;

/// <summary>
/// EF Core 数据上下文（PostgreSQL）。
/// </summary>
public class AppDbContext : Microsoft.EntityFrameworkCore.DbContext
{
    public AppDbContext(DbContextOptions<AppDbContext> options) : base(options) { }

    public DbSet<User> Users => Set<User>();
    public DbSet<Account> Accounts => Set<Account>();
    public DbSet<Device> Devices => Set<Device>();
    public DbSet<Subject> Subjects => Set<Subject>();
    public DbSet<Deck> Decks => Set<Deck>();
    public DbSet<Word> Words => Set<Word>();
    public DbSet<LearningRecord> LearningRecords => Set<LearningRecord>();
    public DbSet<OtaPackage> OtaPackages => Set<OtaPackage>();

    protected override void OnModelCreating(ModelBuilder modelBuilder)
    {
        base.OnModelCreating(modelBuilder);

        // User
        modelBuilder.Entity<User>(e =>
        {
            e.HasIndex(u => u.Username).IsUnique();
            e.Property(u => u.Username).HasMaxLength(64).IsRequired();
            e.Property(u => u.PasswordHash).HasMaxLength(256).IsRequired();
        });

        // Account（v1.5 T5.3 轻账户，与 User 分表——决策 §三.5）
        modelBuilder.Entity<Account>(e =>
        {
            e.HasIndex(a => a.Username).IsUnique();
            e.Property(a => a.Username).HasMaxLength(64).IsRequired();
            e.Property(a => a.PasswordHash).HasMaxLength(256).IsRequired();
            e.Property(a => a.DisplayName).HasMaxLength(64);
        });

        // Device
        modelBuilder.Entity<Device>(e =>
        {
            e.HasIndex(d => d.MacAddress).IsUnique();
            e.HasIndex(d => d.ApiKey).IsUnique();
            e.Property(d => d.MacAddress).HasMaxLength(32).IsRequired();
            e.Property(d => d.ApiKey).HasMaxLength(64).IsRequired();
        });

        // Subject / Deck（v1.4 T4.1 全科地基）
        // Word 侧纯 Id 关联不建 FK/导航（导出 join 显式写）；存量库补丁
        // 同步建表（见 Program.cs SQL 段），列集与新库 EnsureCreated 一致。
        modelBuilder.Entity<Subject>(e =>
        {
            e.HasIndex(s => s.Code).IsUnique();
            e.Property(s => s.Code).HasMaxLength(16).IsRequired();
            e.Property(s => s.Name).HasMaxLength(64).IsRequired();
        });
        modelBuilder.Entity<Deck>(e =>
        {
            e.HasIndex(d => new { d.SubjectId, d.Code }).IsUnique();
            e.Property(d => d.Code).HasMaxLength(16).IsRequired();
            e.Property(d => d.Name).HasMaxLength(128).IsRequired();
            e.Property(d => d.PayloadType).HasMaxLength(16).IsRequired();
            e.Property(d => d.Description).HasMaxLength(512);
            e.HasOne(d => d.Subject).WithMany(s => s.Decks)
             .HasForeignKey(d => d.SubjectId).OnDelete(DeleteBehavior.Restrict);
            e.HasIndex(d => d.OwnerId); // v1.5 T5.3 轻账户归属（null=官方）
        });

        // Word
        modelBuilder.Entity<Word>(e =>
        {
            e.HasIndex(w => new { w.Text, w.Tag }).IsUnique();
            e.HasIndex(w => w.Version);
            e.HasIndex(w => new { w.Archived, w.Tag });
            e.HasIndex(w => w.SubjectId); // T4.1 全科归属
            e.HasIndex(w => w.DeckId);
            e.Property(w => w.Text).HasMaxLength(128).IsRequired();
            e.Property(w => w.Meaning).HasMaxLength(512);
            e.Property(w => w.Phonetic).HasMaxLength(128);
            e.Property(w => w.Audio).HasMaxLength(256);
            e.Property(w => w.Tag).HasMaxLength(64);
            e.Property(w => w.Front).HasMaxLength(512);  // v2 卡面（qa/poem 题面/上句）
            e.Property(w => w.Back).HasMaxLength(1024); // v2 卡面（答案/下句）
        });

        // LearningRecord
        modelBuilder.Entity<LearningRecord>(e =>
        {
            e.HasIndex(lr => new { lr.DeviceId, lr.WordId }).IsUnique();
            e.HasIndex(lr => lr.NextReview);
            e.HasIndex(lr => lr.ConsecutiveWrong); // 错词本排行过滤（P1）
            e.HasOne(lr => lr.Device)
             .WithMany(d => d.LearningRecords)
             .HasForeignKey(lr => lr.DeviceId)
             .OnDelete(DeleteBehavior.Cascade);
            e.HasOne(lr => lr.Word)
             .WithMany(w => w.LearningRecords)
             .HasForeignKey(lr => lr.WordId)
             .OnDelete(DeleteBehavior.Restrict);
        });

        // OtaPackage
        modelBuilder.Entity<OtaPackage>(e =>
        {
            e.HasIndex(o => new { o.TargetBoard, o.Version });
            e.Property(o => o.Version).HasMaxLength(32).IsRequired();
        });

        // 全局查询过滤：软删除（逐实体显式设置，避免非泛型委托推断问题）
        modelBuilder.Entity<User>().HasQueryFilter(e => !e.IsDeleted);
        modelBuilder.Entity<Account>().HasQueryFilter(e => !e.IsDeleted);
        modelBuilder.Entity<Device>().HasQueryFilter(e => !e.IsDeleted);
        modelBuilder.Entity<Subject>().HasQueryFilter(e => !e.IsDeleted);
        modelBuilder.Entity<Deck>().HasQueryFilter(e => !e.IsDeleted);
        modelBuilder.Entity<Word>().HasQueryFilter(e => !e.IsDeleted);
        modelBuilder.Entity<LearningRecord>().HasQueryFilter(e => !e.IsDeleted);
        modelBuilder.Entity<OtaPackage>().HasQueryFilter(e => !e.IsDeleted);
    }
}
