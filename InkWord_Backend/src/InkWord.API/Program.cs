using Hangfire;
using Hangfire.PostgreSql;
using InkWord.API.Filters;
using InkWord.API.Middlewares;
using InkWord.Core.Repositories;
using InkWord.Infrastructure.Cache;
using InkWord.Infrastructure.DbContext;
using InkWord.Infrastructure.Repositories;
using InkWord.Jobs;
using InkWord.Services;
using Microsoft.AspNetCore.Authentication.JwtBearer;
using Microsoft.EntityFrameworkCore;
using Microsoft.Extensions.AI;
using Microsoft.IdentityModel.Tokens;
using Microsoft.OpenApi.Models;
using OpenAI;
using Serilog;
using StackExchange.Redis;
using System.ClientModel;
using System.Text;

var builder = WebApplication.CreateBuilder(args);

// ---- Serilog ----
Log.Logger = new LoggerConfiguration()
    .ReadFrom.Configuration(builder.Configuration)
    .Enrich.FromLogContext()
    .WriteTo.Console()
    .WriteTo.File("logs/inkword-.log", rollingInterval: RollingInterval.Day)
    .CreateLogger();
builder.Host.UseSerilog();

// ---- EF Core (PostgreSQL) ----
builder.Services.AddDbContext<AppDbContext>(opt =>
    opt.UseNpgsql(builder.Configuration.GetConnectionString("Postgres")));

// ---- Redis ----
var redisConn = builder.Configuration.GetConnectionString("Redis") ?? "localhost:6379";
builder.Services.AddSingleton<IConnectionMultiplexer>(_ => ConnectionMultiplexer.Connect(redisConn));
builder.Services.AddStackExchangeRedisCache(opt => opt.Configuration = redisConn);
builder.Services.AddSingleton<IRedisCache, RedisCache>();

// ---- Repositories ----
builder.Services.AddScoped<IWordRepository, WordRepository>();
builder.Services.AddScoped<IDeviceRepository, DeviceRepository>();
builder.Services.AddScoped<IUserRepository, UserRepository>();
builder.Services.AddScoped<ILearningRecordRepository, LearningRecordRepository>();
builder.Services.AddScoped<IOtaPackageRepository, OtaPackageRepository>();

// ---- Services ----
builder.Services.AddScoped<SrsService>();
builder.Services.AddScoped<FsrsService>();
builder.Services.AddScoped<AiContentService>();
builder.Services.AddScoped<PronunciationService>();

// ---- AI IChatClient（M1 路径 B，2026-08-22）：按 Ai:Provider 装配具体实现 ----
// 本地 Ollama（默认）/ OpenAI 兼容云 API 可切换；仅 API 层持有实现包，
// AiContentService 面向 IChatClient 抽象（InkWord.Services 零具体依赖）
var aiCfg = builder.Configuration.GetSection("Ai");
var aiModel = aiCfg["Model"] ?? "qwen2.5:7b";
if (string.Equals(aiCfg["Provider"], "openai", StringComparison.OrdinalIgnoreCase))
{
    var apiKey = aiCfg["CloudApiKey"];
    if (string.IsNullOrEmpty(apiKey))
        throw new InvalidOperationException("Ai:Provider=openai 需配置 Ai:CloudApiKey");
    // 云端 OpenAI 兼容端点（空则官方 api.openai.com）：
    // DeepSeek https://api.deepseek.com/v1 ｜ Qwen 兼容模式 https://dashscope.aliyuncs.com/compatible-mode/v1
    // ｜ 智谱 https://open.bigmodel.cn/api/paas/v4 —— key 走环境变量/user-secrets，不入仓库
    var endpoint = aiCfg["CloudEndpoint"];
    var oaClient = string.IsNullOrEmpty(endpoint)
        ? new OpenAIClient(apiKey)
        : new OpenAIClient(new ApiKeyCredential(apiKey),
            new OpenAIClientOptions { Endpoint = new Uri(endpoint) });
    builder.Services.AddSingleton<IChatClient>(_ => oaClient.GetChatClient(aiModel).AsIChatClient());
}
else
{
    builder.Services.AddSingleton<IChatClient>(_ => new OllamaChatClient(
        new Uri(aiCfg["OllamaUrl"] ?? "http://localhost:11434"), aiModel, new HttpClient()));
}

// ---- Hangfire 任务类（需 DI 注入） ----
builder.Services.AddTransient<DailyPushJob>();
builder.Services.AddTransient<CleanupJob>();
builder.Services.AddTransient<AiContentJob>();

// ---- 设备认证过滤器 ----
builder.Services.AddScoped<DeviceAuthFilter>();

// ---- JWT 认证 ----
var jwtCfg = builder.Configuration.GetSection("Jwt");
builder.Services.AddAuthentication(JwtBearerDefaults.AuthenticationScheme)
    .AddJwtBearer(opt =>
    {
        opt.TokenValidationParameters = new TokenValidationParameters
        {
            ValidateIssuer = true,
            ValidateAudience = true,
            ValidateLifetime = true,
            ValidateIssuerSigningKey = true,
            ValidIssuer = jwtCfg["Issuer"],
            ValidAudience = jwtCfg["Audience"],
            IssuerSigningKey = new SymmetricSecurityKey(Encoding.UTF8.GetBytes(jwtCfg["Secret"]!))
        };
    });

builder.Services.AddControllers();
builder.Services.AddEndpointsApiExplorer();

// ---- Swagger（带 JWT 锁头按钮） ----
builder.Services.AddSwaggerGen(c =>
{
    c.SwaggerDoc("v1", new OpenApiInfo { Title = "InkWord API", Version = "v1" });
    c.AddSecurityDefinition("Bearer", new OpenApiSecurityScheme
    {
        Description = "JWT 授权头。示例：Bearer {token}",
        Name = "Authorization",
        In = ParameterLocation.Header,
        Type = SecuritySchemeType.ApiKey,
        Scheme = "Bearer"
    });
    c.AddSecurityRequirement(new OpenApiSecurityRequirement
    {
        {
            new OpenApiSecurityScheme
            {
                Reference = new OpenApiReference { Type = ReferenceType.SecurityScheme, Id = "Bearer" }
            }, Array.Empty<string>()
        }
    });
});

// ---- CORS（供 Angular 前端） ----
builder.Services.AddCors(opt => opt.AddDefaultPolicy(p =>
    p.AllowAnyOrigin().AllowAnyHeader().AllowAnyMethod()));

// ---- Hangfire ----
builder.Services.AddHangfire(config => config
    .UseSimpleAssemblyNameTypeSerializer()
    .UseRecommendedSerializerSettings()
    .UsePostgreSqlStorage(c => c.UseNpgsqlConnection(
        builder.Configuration.GetConnectionString("Postgres"))));
builder.Services.AddHangfireServer();

var app = builder.Build();

// ---- 全局异常中间件（最先注册） ----
app.UseMiddleware<ExceptionMiddleware>();

app.UseSwagger();
app.UseSwaggerUI();
app.UseCors();
app.UseAuthentication();
app.UseAuthorization();

// 自动建表 + 种子（开发环境）：必须在 JobRegistrar.Register() 之前 ——
// Hangfire 初始化会先建 hangfire 表，库非空后 EnsureCreated 按语义
// 直接跳过建表，导致业务表缺失（2026-08-20 实测修复）
if (app.Environment.IsDevelopment())
{
    using var scope = app.Services.CreateScope();
    var db = scope.ServiceProvider.GetRequiredService<AppDbContext>();
    db.Database.EnsureCreated();

    // EnsureCreated 不改已有表：Words 四字段（V2.1 词库扩展 2026-08-20）
    // 幂等补列；新库建表已含，此段空转。正式迁移机制（EF Migrations）
    // 引入后删除本补丁。
    var addCols = new[]
    {
        // V2.1 词库扩展（2026-08-20）
        "ALTER TABLE \"Words\" ADD COLUMN IF NOT EXISTS \"Root\" text NOT NULL DEFAULT ''",
        "ALTER TABLE \"Words\" ADD COLUMN IF NOT EXISTS \"Inflections\" text NOT NULL DEFAULT ''",
        "ALTER TABLE \"Words\" ADD COLUMN IF NOT EXISTS \"Source\" text NOT NULL DEFAULT ''",
        "ALTER TABLE \"Words\" ADD COLUMN IF NOT EXISTS \"Grade\" text NOT NULL DEFAULT ''",
        // AI 词库增强（M1 路径 B）：AiStatus 0 未生成/1 待审/2 已应用/3 失败
        "ALTER TABLE \"Words\" ADD COLUMN IF NOT EXISTS \"AiStatus\" integer NOT NULL DEFAULT 0",
        "ALTER TABLE \"Words\" ADD COLUMN IF NOT EXISTS \"AiSuggestion\" text",
        // FSRS 影子列（M3 路径 A）+ 发音评分（M5 路径 C）
        "ALTER TABLE \"LearningRecords\" ADD COLUMN IF NOT EXISTS \"FsrsStability\" double precision NOT NULL DEFAULT 0",
        "ALTER TABLE \"LearningRecords\" ADD COLUMN IF NOT EXISTS \"FsrsDifficulty\" double precision NOT NULL DEFAULT 0",
        "ALTER TABLE \"LearningRecords\" ADD COLUMN IF NOT EXISTS \"FsrsNextReview\" timestamptz",
        "ALTER TABLE \"LearningRecords\" ADD COLUMN IF NOT EXISTS \"LastPronScore\" integer",
    };
    foreach (var sql in addCols)
        db.Database.ExecuteSqlRaw(sql);

    // 管理端无注册入口（AuthController 仅登录）：首次启动种子默认账号
    // admin/admin123（仅 Development；生产应手动 SQL 重置或接环境变量）
    if (!db.Users.Any())
    {
        db.Users.Add(new InkWord.Core.Entities.User
        {
            Username = "admin",
            PasswordHash = InkWord.API.Controllers.AuthController.HashPassword("admin123"),
            DisplayName = "Administrator",
            Role = "Admin",
        });
        await db.SaveChangesAsync();
    }
}

// ---- 系统默认词库种子（2026-08-23）：Words 表为空时自动导入
// SeedData/default_words.csv（开源中小学词库/古诗词 ≈2400 条，设备端
// 全库同步上限 4000，见 tools/default_vocab/README.md）。开发/生产一致
// 生效；表未就绪或文件缺失仅告警不阻断启动。
using (var seedScope = app.Services.CreateScope())
{
    try
    {
        var seedDb = seedScope.ServiceProvider
            .GetRequiredService<AppDbContext>();
        var seeded = await InkWord.API.DefaultWordSeeder.SeedAsync(seedDb);
        if (seeded > 0)
            Log.Information("默认词库已导入 {Count} 条", seeded);
    }
    catch (Exception ex)
    {
        Log.Warning(ex, "默认词库种子跳过（表未就绪或文件缺失）");
    }
}

// ---- Hangfire Dashboard ----
app.UseHangfireDashboard("/hangfire");
JobRegistrar.Register();

app.MapControllers();

app.Run();
