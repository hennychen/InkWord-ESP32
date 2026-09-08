using System.Data.Common;
using InkWord.Infrastructure.DbContext;
using Microsoft.AspNetCore.Authentication;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Mvc.Testing;
using Microsoft.AspNetCore.TestHost;
using Microsoft.Data.Sqlite;
using Microsoft.EntityFrameworkCore;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using System.Security.Claims;
using System.Text.Encodings.Web;

namespace InkWord.Tests;

/// <summary>
/// 集成测试 WebApplicationFactory（A4 基建，2026-09-08）：
/// - 替换 AppDbContext 为 Sqlite（内存模式不支持迁移/复杂查询，Sqlite 更贴近生产）
/// - 替换认证为测试 stub（绕过 JWT 校验，直接注入测试身份）
/// - 每测试类独立 Sqlite 连接（测试间隔离）
/// </summary>
public class TestWebApplicationFactory : WebApplicationFactory<Program>
{
    private readonly SqliteConnection _connection;

    public TestWebApplicationFactory()
    {
        _connection = new SqliteConnection("DataSource=:memory:");
        _connection.Open();
    }

    protected override void ConfigureWebHost(IWebHostBuilder builder)
    {
        // 测试环境：跳过 Program.cs 的 MigrateInkWordDatabase（PostgreSQL 语法，Sqlite 不兼容）
        // EnsureCreated 已建表，补丁不需要
        builder.UseEnvironment("Testing");

        builder.ConfigureTestServices(services =>
        {
            // 移除原 AppDbContext 注册（PostgreSQL/SqlServer）
            var descriptor = services.SingleOrDefault(d => d.ServiceType == typeof(DbContextOptions<AppDbContext>));
            if (descriptor != null) services.Remove(descriptor);

            // 替换为 Sqlite 内存连接
            services.AddDbContext<AppDbContext>(options =>
            {
                options.UseSqlite(_connection);
            });

            // 移除原认证（JWT Bearer），替换为测试 stub
            var authDescriptor = services.SingleOrDefault(d => d.ServiceType == typeof(IAuthenticationSchemeProvider));
            if (authDescriptor != null)
            {
                // 移除所有认证相关注册
                var authDescriptors = services.Where(d =>
                    d.ServiceType.FullName?.Contains("Authentication") == true ||
                    d.ServiceType == typeof(IAuthenticationSchemeProvider) ||
                    d.ServiceType == typeof(IAuthenticationService)).ToList();
                foreach (var d in authDescriptors) services.Remove(d);
            }

            // 添加测试认证 stub：所有请求视为已认证（身份由测试手动注入）
            // 设为默认 scheme，控制器 [Authorize] 无参时自动使用
            services.AddAuthentication(options =>
            {
                options.DefaultAuthenticateScheme = "Test";
                options.DefaultChallengeScheme = "Test";
            })
                .AddScheme<AuthenticationSchemeOptions, TestAuthHandler>("Test", _ => { });

            // 确保数据库建表
            using var scope = services.BuildServiceProvider().CreateScope();
            var db = scope.ServiceProvider.GetRequiredService<AppDbContext>();
            db.Database.EnsureCreated();
        });
    }

    /// <summary>创建带测试身份的 HttpClient（绕过 JWT，直接注入 Claims）</summary>
    public HttpClient CreateAuthenticatedClient(string username = "testuser", string role = "Admin")
    {
        var client = CreateClient();
        client.DefaultRequestHeaders.Add("X-Test-User", username);
        client.DefaultRequestHeaders.Add("X-Test-Role", role);
        return client;
    }

    protected override void Dispose(bool disposing)
    {
        base.Dispose(disposing);
        if (disposing) _connection?.Dispose();
    }
}

/// <summary>测试认证处理器：从请求头读 X-Test-User/X-Test-Role 注入 Claims</summary>
public class TestAuthHandler : AuthenticationHandler<AuthenticationSchemeOptions>
{
    public TestAuthHandler(
        IOptionsMonitor<AuthenticationSchemeOptions> options,
        ILoggerFactory logger,
        UrlEncoder encoder) : base(options, logger, encoder) { }

    protected override Task<AuthenticateResult> HandleAuthenticateAsync()
    {
        var username = Request.Headers["X-Test-User"].FirstOrDefault();
        var role = Request.Headers["X-Test-Role"].FirstOrDefault() ?? "Admin";

        if (string.IsNullOrEmpty(username))
            return Task.FromResult(AuthenticateResult.NoResult());

        var claims = new[]
        {
            new Claim(ClaimTypes.Name, username),
            new Claim(ClaimTypes.Role, role),
        };
        var identity = new ClaimsIdentity(claims, "Test");
        var principal = new ClaimsPrincipal(identity);
        var ticket = new AuthenticationTicket(principal, "Test");

        return Task.FromResult(AuthenticateResult.Success(ticket));
    }
}
