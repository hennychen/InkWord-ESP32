using System.IdentityModel.Tokens.Jwt;
using System.Security.Claims;
using InkWord.API.Controllers;
using InkWord.Core.Entities;
using Microsoft.Extensions.Configuration;
using Xunit;

namespace InkWord.Tests;

/// <summary>
/// 轻账户纯逻辑测试（v1.5 T5.3，ACCOUNT_MODEL_DECISION §四）。
///
/// PBKDF2 哈希往返 / learner JWT claims（role=learner + NameIdentifier
/// 归属标识，管理端路径无 nameid）/ 条目映射（FillWord：Text/Front 双写、
/// 截断与 trim——设备 WordEntry 语义泛化契约）。Web 层集成（端点鉴权、
/// 归属校验）依赖 PostgreSQL + 认证管线，不在本套件（现有测试均为纯
/// 服务直测范式）。
/// </summary>
public class AccountTests
{
    private static IConfiguration TestCfg() => new ConfigurationBuilder()
        .AddInMemoryCollection(new Dictionary<string, string?>
        {
            ["Jwt:Secret"] = "test-secret-key-at-least-32-chars-long!!",
            ["Jwt:Issuer"] = "InkWord.Tests",
            ["Jwt:Audience"] = "InkWord.Clients",
        })
        .Build();

    // ---- PBKDF2 哈希（Account/User 同源） ----

    [Fact]
    public void HashPassword_Roundtrip()
    {
        var hash = AuthController.HashPassword("s3cret!");
        Assert.True(AuthController.VerifyPassword("s3cret!", hash));
    }

    [Fact]
    public void HashPassword_WrongPassword_Rejected()
    {
        var hash = AuthController.HashPassword("s3cret!");
        Assert.False(AuthController.VerifyPassword("s3cret", hash));
        Assert.False(AuthController.VerifyPassword("", hash));
    }

    [Fact]
    public void HashPassword_RandomSalt_TwoHashesDiffer()
    {
        var a = AuthController.HashPassword("same-input");
        var b = AuthController.HashPassword("same-input");
        Assert.NotEqual(a, b);                          // 随机 salt
        Assert.True(AuthController.VerifyPassword("same-input", a));
        Assert.True(AuthController.VerifyPassword("same-input", b));
    }

    [Fact]
    public void VerifyPassword_MalformedHash_Rejected()
    {
        Assert.False(AuthController.VerifyPassword("x", "not-a-hash"));
        Assert.False(AuthController.VerifyPassword("x", ""));
    }

    // ---- learner JWT（role 区分 + 归属标识） ----

    [Fact]
    public void IssueToken_Learner_HasRoleAndAccountId()
    {
        var accountId = Guid.NewGuid();
        var token = AuthController.IssueToken(TestCfg(), "alice", "learner", accountId);

        var jwt = new JwtSecurityTokenHandler().ReadJwtToken(token);
        // JwtSecurityToken 直构不走出站映射，claim 保持 ClaimTypes 长名；
        // 运行时 JwtBearer 入站映射回 ClaimTypes.*（MapInboundClaims 默认），
        // [Authorize(Roles)] 与 FindFirstValue(ClaimTypes.NameIdentifier) 正常
        Assert.Equal("learner",
            jwt.Claims.First(c => c.Type == ClaimTypes.Role).Value);
        Assert.Equal("alice", jwt.Claims.First(c => c.Type == ClaimTypes.Name).Value);
        Assert.Equal(accountId.ToString(),
            jwt.Claims.First(c => c.Type == ClaimTypes.NameIdentifier).Value);
    }

    [Fact]
    public void IssueToken_AdminPath_NoAccountIdClaim()
    {
        // 管理端登录不传 accountId：role 取用户表值（Admin/Operator），
        // 白名单外的 learner token 无法过 Admin 控制器
        var token = AuthController.IssueToken(TestCfg(), "admin", "Admin");

        var jwt = new JwtSecurityTokenHandler().ReadJwtToken(token);
        Assert.Equal("Admin", jwt.Claims.First(c => c.Type == ClaimTypes.Role).Value);
        Assert.DoesNotContain(jwt.Claims, c => c.Type == ClaimTypes.NameIdentifier);
    }

    // ---- 条目映射（设备 WordEntry 语义泛化契约） ----

    [Fact]
    public void FillWord_TextFrontDualWrite_WithTrim()
    {
        var w = new Word();
        MyDeckController.FillWord(w, "  apple ", "苹果", "/ˈæpl/", "an apple a day");

        Assert.Equal("apple", w.Text);
        Assert.Equal("apple", w.Front);      // v2 卡面双写（T4.1 契约）
        Assert.Equal("苹果", w.Back);
        Assert.Equal("苹果", w.Meaning);
        Assert.Equal("/ˈæpl/", w.Phonetic);
        Assert.Equal("an apple a day", w.Example);
    }

    [Fact]
    public void FillWord_LongFront_TruncatedTo128()
    {
        var longFront = new string('x', 300);
        var w = new Word();
        MyDeckController.FillWord(w, longFront, "背面", null, null);

        Assert.Equal(128, w.Text.Length);
        Assert.Equal(128, w.Front.Length);
    }

    [Fact]
    public void FillWord_NullOptional_FieldsEmpty()
    {
        var w = new Word();
        MyDeckController.FillWord(w, "床前明月光", "疑是地上霜", null, null);

        // poem-card：text=上句 / meaning=下句（qa 同构：题面/答案）
        Assert.Equal("床前明月光", w.Text);
        Assert.Equal("疑是地上霜", w.Meaning);
        Assert.Equal("", w.Phonetic);
        Assert.Equal("", w.Example);
    }

    // ---- 条目 Version 递增（P3 管理端条目 CRUD 与 me 端同源公共方法） ----

    [Theory]
    [InlineData(5, 100, 101)]   // 全局 max 更高 → 接全局 max 递增（设备增量同步契约：新 Version 须高于任一设备已拉游标）
    [InlineData(100, 5, 101)]   // 自身更高（异常防回退）→ 自身 +1
    [InlineData(50, 50, 51)]    // 相等 → +1
    [InlineData(0, 0, 1)]       // 冷启动首条 → 1
    public void NextVersion_TakesMaxOfCurrentAndGlobal(int current, int globalMax, int expected)
        => Assert.Equal(expected, MyDeckController.NextVersion(current, globalMax));
}
