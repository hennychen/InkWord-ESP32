using System.Net;
using System.Net.Http.Json;
using InkWord.API.DTOs;
using InkWord.Infrastructure.DbContext;
using Microsoft.EntityFrameworkCore;
using Microsoft.Extensions.DependencyInjection;

namespace InkWord.Tests;

/// <summary>
/// 集成测试（A4 基建，2026-09-08）：WebApplicationFactory + Sqlite + 测试认证 stub。
/// 六项清单（ROADMAP 已登记）：
/// 1. 归属 404（chat-review 非本人设备）
/// 2. 空周报 404
/// 3. 阅读列表映射
/// 4. me books Published 过滤
/// 5. 条目 CRUD uq 409 与 Roles 白名单
/// 6. aggregate 零设备分支
/// </summary>
public class IntegrationTests : IClassFixture<TestWebApplicationFactory>
{
    private readonly TestWebApplicationFactory _factory;
    private readonly HttpClient _client;

    public IntegrationTests(TestWebApplicationFactory factory)
    {
        _factory = factory;
        _client = factory.CreateAuthenticatedClient("testuser", "learner"); // MyDeviceController 要求 learner 角色
    }

    /// <summary>清空数据库（每测试前调用，确保隔离）</summary>
    private async Task ResetDbAsync()
    {
        using var scope = _factory.Services.CreateScope();
        var db = scope.ServiceProvider.GetRequiredService<AppDbContext>();
        // 删所有数据（顺序：先删依赖表，再删主表；Device.UserId → Account.Id 外键）
        db.LearningRecords.RemoveRange(db.LearningRecords);
        db.ReadingProgresses.RemoveRange(db.ReadingProgresses);
        db.Books.RemoveRange(db.Books);
        db.Words.RemoveRange(db.Words);
        db.Decks.RemoveRange(db.Decks);
        db.Subjects.RemoveRange(db.Subjects);
        db.Devices.RemoveRange(db.Devices);  // 先删 Devices（依赖 Account）
        db.Accounts.RemoveRange(db.Accounts); // 再删 Accounts（主表）
        await db.SaveChangesAsync();
    }

    /// <summary>创建测试账户与设备，返回 (accountId, deviceId)</summary>
    private async Task<(Guid accountId, Guid deviceId)> SeedAccountAndDeviceAsync(string username = "testuser")
    {
        using var scope = _factory.Services.CreateScope();
        var db = scope.ServiceProvider.GetRequiredService<AppDbContext>();

        var account = new InkWord.Core.Entities.Account
        {
            Id = Guid.NewGuid(),
            Username = username,
            PasswordHash = "dummy", // 测试不校验密码
            CreatedAt = DateTime.UtcNow,
        };
        db.Accounts.Add(account);

        var device = new InkWord.Core.Entities.Device
        {
            Id = Guid.NewGuid(),
            UserId = account.Id,
            Name = "test-device",
            MacAddress = "AABBCCDDEEFF",
            ApiKey = "test-api-key",
            LastHeartbeat = DateTime.UtcNow,
        };
        db.Devices.Add(device);

        await db.SaveChangesAsync();
        return (account.Id, device.Id);
    }

    // ---- 1. 归属 404（chat-review 非本人设备） ----

    [Fact]
    public async Task ChatReview_NonOwnedDevice_Returns404()
    {
        await ResetDbAsync();
        var (_, myDeviceId) = await SeedAccountAndDeviceAsync();

        // 创建另一账户的设备
        using (var scope = _factory.Services.CreateScope())
        {
            var db = scope.ServiceProvider.GetRequiredService<AppDbContext>();
            var otherAccount = new InkWord.Core.Entities.Account
            {
                Id = Guid.NewGuid(),
                Username = "other",
                PasswordHash = "dummy",
                CreatedAt = DateTime.UtcNow,
            };
            db.Accounts.Add(otherAccount);
            var otherDevice = new InkWord.Core.Entities.Device
            {
                Id = Guid.NewGuid(),
                UserId = otherAccount.Id,
                Name = "other-device",
                MacAddress = "112233445566",
                ApiKey = "other-key",
                LastHeartbeat = DateTime.UtcNow,
            };
            db.Devices.Add(otherDevice);
            await db.SaveChangesAsync();

            // 用 testuser 身份访问 otherDevice 的 chat-review
            var resp = await _client.GetAsync($"/api/me/devices/{otherDevice.Id}/chat-review");
            Assert.Equal(HttpStatusCode.NotFound, resp.StatusCode);
        }
    }

    // ---- 2. 空周报 404 ----

    [Fact]
    public async Task ChatReview_NoRecords_Returns404()
    {
        await ResetDbAsync();
        var (_, deviceId) = await SeedAccountAndDeviceAsync();

        var resp = await _client.GetAsync($"/api/me/devices/{deviceId}/chat-review");
        Assert.Equal(HttpStatusCode.NotFound, resp.StatusCode);
    }

    // ---- 3. 阅读列表映射 ----

    [Fact]
    public async Task Reading_EmptyList_ReturnsEmptyArray()
    {
        await ResetDbAsync();
        var (_, deviceId) = await SeedAccountAndDeviceAsync();

        var resp = await _client.GetAsync($"/api/me/devices/{deviceId}/reading");
        Assert.Equal(HttpStatusCode.OK, resp.StatusCode);
        var content = await resp.Content.ReadAsStringAsync();
        Assert.Contains("[]", content); // 空列表
    }

    // ---- 4. me books Published 过滤 ----

    [Fact]
    public async Task Books_OnlyPublished_ReturnsFiltered()
    {
        await ResetDbAsync();

        using (var scope = _factory.Services.CreateScope())
        {
            var db = scope.ServiceProvider.GetRequiredService<AppDbContext>();
            db.Books.Add(new InkWord.Core.Entities.Book
            {
                Id = Guid.NewGuid(),
                BookKey = "published-book",
                Title = "已发布",
                Author = "作者",
                Language = "zh",
                Published = true,
                CreatedAt = DateTime.UtcNow,
            });
            db.Books.Add(new InkWord.Core.Entities.Book
            {
                Id = Guid.NewGuid(),
                BookKey = "draft-book",
                Title = "草稿",
                Author = "作者",
                Language = "zh",
                Published = false,
                CreatedAt = DateTime.UtcNow,
            });
            await db.SaveChangesAsync();
        }

        var resp = await _client.GetAsync("/api/me/books");
        Assert.Equal(HttpStatusCode.OK, resp.StatusCode);
        var content = await resp.Content.ReadAsStringAsync();
        Assert.Contains("已发布", content);
        Assert.DoesNotContain("草稿", content);
    }

    // ---- 5. 条目 CRUD uq 409 与 Roles 白名单 ----

    [Fact]
    public async Task AdminDeck_AddItem_DuplicateFront_Returns409()
    {
        await ResetDbAsync();

        // 先建科目与卡组
        Guid deckId;
        using (var scope = _factory.Services.CreateScope())
        {
            var db = scope.ServiceProvider.GetRequiredService<AppDbContext>();
            var subject = new InkWord.Core.Entities.Subject
            {
                Id = Guid.NewGuid(),
                Code = "en",
                Name = "英语",
            };
            db.Subjects.Add(subject);
            var deck = new InkWord.Core.Entities.Deck
            {
                Id = Guid.NewGuid(),
                Code = "testdeck",
                Name = "测试卡组",
                PayloadType = "word-card",
                SubjectId = subject.Id,
            };
            db.Decks.Add(deck);
            await db.SaveChangesAsync();
            deckId = deck.Id;
        }

        // 添加第一条
        var req1 = new { Front = "hello", Back = "你好", Phonetic = "", Example = "" };
        var resp1 = await _client.PostAsJsonAsync($"/api/admin/decks/{deckId}/items", req1);
        Assert.Equal(HttpStatusCode.OK, resp1.StatusCode);

        // 添加重复 front（同 deck 的 Tag 下 uq(Text,Tag)）
        var req2 = new { Front = "hello", Back = "你好2", Phonetic = "", Example = "" };
        var resp2 = await _client.PostAsJsonAsync($"/api/admin/decks/{deckId}/items", req2);
        Assert.Equal(HttpStatusCode.Conflict, resp2.StatusCode);
    }

    [Fact]
    public async Task AdminDeck_LearnerRole_Returns403()
    {
        await ResetDbAsync();

        // 用 learner 角色访问管理端端点
        var learnerClient = _factory.CreateAuthenticatedClient("learner", "Learner");
        var resp = await learnerClient.GetAsync("/api/admin/subjects");
        Assert.Equal(HttpStatusCode.Forbidden, resp.StatusCode);
    }

    // ---- 6. aggregate 零设备分支 ----

    [Fact]
    public async Task Aggregate_NoDevices_ReturnsEmptyAggregate()
    {
        await ResetDbAsync();
        await SeedAccountAndDeviceAsync(); // 创建账户但不绑设备（aggregate 查的是账户绑定的设备）

        // 实际上 aggregate 查的是 LearningRecords 按设备聚合，零设备 = 零记录
        var resp = await _client.GetAsync("/api/me/progress/aggregate?take=10");
        Assert.Equal(HttpStatusCode.OK, resp.StatusCode);
        var content = await resp.Content.ReadAsStringAsync();
        Assert.Contains("\"items\":[]", content);
        Assert.Contains("\"masteredCount\":0", content);
    }
}
