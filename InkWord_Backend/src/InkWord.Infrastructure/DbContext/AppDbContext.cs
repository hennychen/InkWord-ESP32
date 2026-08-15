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
    public DbSet<Device> Devices => Set<Device>();
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

        // Device
        modelBuilder.Entity<Device>(e =>
        {
            e.HasIndex(d => d.MacAddress).IsUnique();
            e.HasIndex(d => d.ApiKey).IsUnique();
            e.Property(d => d.MacAddress).HasMaxLength(32).IsRequired();
            e.Property(d => d.ApiKey).HasMaxLength(64).IsRequired();
        });

        // Word
        modelBuilder.Entity<Word>(e =>
        {
            e.HasIndex(w => new { w.Text, w.Tag }).IsUnique();
            e.HasIndex(w => w.Version);
            e.HasIndex(w => new { w.Archived, w.Tag });
            e.Property(w => w.Text).HasMaxLength(128).IsRequired();
            e.Property(w => w.Meaning).HasMaxLength(512);
            e.Property(w => w.Phonetic).HasMaxLength(128);
            e.Property(w => w.Audio).HasMaxLength(256);
            e.Property(w => w.Tag).HasMaxLength(64);
        });

        // LearningRecord
        modelBuilder.Entity<LearningRecord>(e =>
        {
            e.HasIndex(lr => new { lr.DeviceId, lr.WordId }).IsUnique();
            e.HasIndex(lr => lr.NextReview);
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
        modelBuilder.Entity<Device>().HasQueryFilter(e => !e.IsDeleted);
        modelBuilder.Entity<Word>().HasQueryFilter(e => !e.IsDeleted);
        modelBuilder.Entity<LearningRecord>().HasQueryFilter(e => !e.IsDeleted);
        modelBuilder.Entity<OtaPackage>().HasQueryFilter(e => !e.IsDeleted);
    }
}
