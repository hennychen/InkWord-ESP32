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
using Microsoft.IdentityModel.Tokens;
using Microsoft.OpenApi.Models;
using Serilog;
using StackExchange.Redis;
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

// ---- Hangfire 任务类（需 DI 注入） ----
builder.Services.AddTransient<DailyPushJob>();
builder.Services.AddTransient<CleanupJob>();

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
        "ALTER TABLE \"Words\" ADD COLUMN IF NOT EXISTS \"Root\" text NOT NULL DEFAULT ''",
        "ALTER TABLE \"Words\" ADD COLUMN IF NOT EXISTS \"Inflections\" text NOT NULL DEFAULT ''",
        "ALTER TABLE \"Words\" ADD COLUMN IF NOT EXISTS \"Source\" text NOT NULL DEFAULT ''",
        "ALTER TABLE \"Words\" ADD COLUMN IF NOT EXISTS \"Grade\" text NOT NULL DEFAULT ''",
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

// ---- Hangfire Dashboard ----
app.UseHangfireDashboard("/hangfire");
JobRegistrar.Register();

app.MapControllers();

app.Run();
