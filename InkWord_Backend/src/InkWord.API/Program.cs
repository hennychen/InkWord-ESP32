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
builder.Services.AddScoped<IAccountRepository, AccountRepository>(); // v1.5 T5.3 轻账户
builder.Services.AddScoped<ILearningRecordRepository, LearningRecordRepository>();
builder.Services.AddScoped<IOtaPackageRepository, OtaPackageRepository>();

// ---- Services ----
builder.Services.AddScoped<SrsService>();
builder.Services.AddScoped<FsrsService>();
builder.Services.AddScoped<AiContentService>();
builder.Services.AddScoped<PronunciationService>();
builder.Services.AddScoped<TtsService>();
builder.Services.AddScoped<ChatService>();
builder.Services.AddScoped<VoiceSearchService>(); // 语音查词（2026-08-28，ASR+词库三级匹配）

// ---- ASR 转写引擎（P2A 2026-08-24）：sherpa-onnx C# 绑定（native 随 NuGet
// 分发），模型 volume 挂载（Asr:ModelDir，与 M5 GOP 升级共用本绑定）；
// Asr:Provider=none 时 Transcribe 恒 null，chat 端点 503——不炸启动 ----
builder.Services.AddSingleton<IAsrTranscriber, SherpaAsrService>();

// ---- TTS 合成引擎（P0B 2026-08-24）：Tts:Provider 切换 Piper/云 API，
// 与 Ai:Provider 的 IChatClient 装配同模式；云实现预留（当前仅 piper） ----
if (string.Equals(builder.Configuration["Tts:Provider"], "cloud",
    StringComparison.OrdinalIgnoreCase))
{
    // 云 TTS（OpenAI 兼容 /v1/audio/speech）：一期预留，接入时在此装配
    throw new InvalidOperationException("Tts:Provider=cloud 尚未接入，请使用 piper");
}
builder.Services.AddSingleton<ISpeechSynthesizer, PiperSynthesizer>();

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
builder.Services.AddTransient<TtsJob>();
builder.Services.AddTransient<ChatAudioCleanupJob>();
// A3 对话复盘：落库执行体/清理/周报 Job；Sink 与词库快照 Singleton
// （无状态/自管缓存，经 IServiceScopeFactory 取短生命周期 db）
builder.Services.AddTransient<ChatTurnLogger>();
builder.Services.AddTransient<ChatTurnCleanupJob>();
builder.Services.AddTransient<ChatReviewJob>();
builder.Services.AddSingleton<IChatTurnSink, HangfireChatTurnSink>();
builder.Services.AddSingleton<IWordListProvider, CachedWordListProvider>();

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
        // 全科地基（v1.4 T4.1）：Words 六列（Item 混合模型）
        "ALTER TABLE \"Words\" ADD COLUMN IF NOT EXISTS \"SubjectId\" uuid",
        "ALTER TABLE \"Words\" ADD COLUMN IF NOT EXISTS \"DeckId\" uuid",
        "ALTER TABLE \"Words\" ADD COLUMN IF NOT EXISTS \"Front\" text NOT NULL DEFAULT ''",
        "ALTER TABLE \"Words\" ADD COLUMN IF NOT EXISTS \"Back\" text NOT NULL DEFAULT ''",
        "ALTER TABLE \"Words\" ADD COLUMN IF NOT EXISTS \"PayloadJson\" text",
    };
    foreach (var sql in addCols)
        db.Database.ExecuteSqlRaw(sql);

    // T4.1 全科地基：Subjects/Decks 建表（列集与 EnsureCreated 新库一致；
    // Word 侧纯 Id 关联不建 FK）+ 固定 Guid 种子（en 科目 + junior 默认卡组，
    // 供存量英语词条整体迁移归属）+ 存量回填。全程幂等，新库空转。
    var t41Sql = new[]
    {
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
        // 种子：固定 Guid（导出 v2 映射稳定性不依赖，仅回填/归属一致性）
        @"INSERT INTO ""Subjects"" (""Id"",""Code"",""Name"",""SortOrder"",""CreatedAt"",""IsDeleted"")
           VALUES ('ee000000-0000-0000-0000-000000000001','en','英语',0,now(),false)
           ON CONFLICT (""Code"") DO NOTHING",
        @"INSERT INTO ""Decks"" (""Id"",""SubjectId"",""Code"",""Name"",""PayloadType"",""Description"",""CreatedAt"",""IsDeleted"")
           VALUES ('ee000000-0000-0000-0000-0000000000d1',
                   'ee000000-0000-0000-0000-000000000001','junior','初中英语（默认）','word-card',
                   '存量英语词条整体迁移归属（T4.1）',now(),false)
           ON CONFLICT (""SubjectId"",""Code"") DO NOTHING",
        // 存量回填：英语整体迁为 en 默认卡组 + v2 卡面镜像（幂等：仅补空）
        @"UPDATE ""Words"" SET ""SubjectId""='ee000000-0000-0000-0000-000000000001' WHERE ""SubjectId"" IS NULL",
        @"UPDATE ""Words"" SET ""DeckId""='ee000000-0000-0000-0000-0000000000d1' WHERE ""DeckId"" IS NULL",
        @"UPDATE ""Words"" SET ""Front""=""Text"" WHERE ""Front""=''
           AND ""Text""<>''",
        @"UPDATE ""Words"" SET ""Back""=""Meaning"" WHERE ""Back""=''
           AND ""Meaning""<>''",
    };
    foreach (var sql in t41Sql)
        db.Database.ExecuteSqlRaw(sql);

    // v1.5 T5.3 轻账户（ACCOUNT_MODEL_DECISION §四）：Accounts 建表
    // （与 Users 分表——决策 §三.5）+ Decks.OwnerId 归属列（null=官方）。
    // 全程幂等，新库 EnsureCreated 已含，此段空转。
    var t53Sql = new[]
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
    foreach (var sql in t53Sql)
        db.Database.ExecuteSqlRaw(sql);

    // A3 对话复盘（2026-08-29）：ChatTurns 轮次日志（append-only，
    // 90 天由 ChatTurnCleanupJob 回收）+ ChatReviews 周报留痕（同设备
    // 同周唯一，倒序取最新即本周）。幂等，新库 EnsureCreated 已含空转。
    var a3Sql = new[]
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
    foreach (var sql in a3Sql)
        db.Database.ExecuteSqlRaw(sql);

    // v2.0 T6.2 完整账户（ACCOUNT_MODEL_DECISION §五）：Device.UserId 兑现
    // （纯 Id 关联指向 Accounts，不建 FK）。存量库可能被 Device.User 旧导航
    // 的 EF 惯例生成 Devices→Users FK，动态拆除防写入 Account.Id 违约；
    // UserId 索引服务绑定设备清单/LWS 聚合查询。全程幂等，新库空转。
    var t20Sql = new[]
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
    foreach (var sql in t20Sql)
        db.Database.ExecuteSqlRaw(sql);

    // v2.0 #3 卡组生态首增量（UGC 分享）：Decks 分享开关两列 +
    // 部分索引（仅已分享行，发现页 IsShared 查询低开销）。
    // 全程幂等，新库 EnsureCreated 已含，此段空转。
var t21Sql = new[]
    {
        "ALTER TABLE \"Decks\" ADD COLUMN IF NOT EXISTS \"IsShared\" boolean NOT NULL DEFAULT false",
        "ALTER TABLE \"Decks\" ADD COLUMN IF NOT EXISTS \"SharedAt\" timestamp with time zone",
        "CREATE INDEX IF NOT EXISTS \"IX_Decks_IsShared\" ON \"Decks\" (\"IsShared\") WHERE \"IsShared\"",
    };
foreach (var sql in t21Sql)
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

// 语文古诗文 Deck 种子（v1.4 T4.4）：按卡组幂等（Decks 含 poems 即跳过），
// 英语库存在与否均可补第二科目；同步协议 v2 随增量下发
using (var poemScope = app.Services.CreateScope())
{
    try
    {
        var poemDb = poemScope.ServiceProvider
            .GetRequiredService<AppDbContext>();
        var poems = await InkWord.API.PoemSeeder.SeedAsync(poemDb);
        if (poems > 0)
            Log.Information("古诗 Deck 已导入 {Count} 条", poems);
    }
    catch (Exception ex)
    {
        Log.Warning(ex, "古诗 Deck 种子跳过（表未就绪或文件缺失）");
    }
}

// ---- Hangfire Dashboard ----
app.UseHangfireDashboard("/hangfire");
JobRegistrar.Register();

app.MapControllers();

app.Run();
