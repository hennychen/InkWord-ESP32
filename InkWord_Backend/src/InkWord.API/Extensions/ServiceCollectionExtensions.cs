using Hangfire;
using Hangfire.PostgreSql;
using InkWord.API.Filters;
using InkWord.Core.Repositories;
using InkWord.Infrastructure.Cache;
using InkWord.Infrastructure.DbContext;
using InkWord.Infrastructure.Repositories;
using InkWord.Jobs;
using InkWord.Services;
using Asp.Versioning;
using Microsoft.AspNetCore.Authentication.JwtBearer;
using Microsoft.EntityFrameworkCore;
using Microsoft.Extensions.AI;
using Microsoft.IdentityModel.Tokens;
using Microsoft.OpenApi.Models;
using OpenAI;
using Serilog;
using Polly;
using Polly.Retry;
using StackExchange.Redis;
using System.ClientModel;
using System.Text;

namespace InkWord.API.Extensions;

/// <summary>
/// DI 注册与服务配置扩展（从 Program.cs 提取，2026-09-05）。
/// </summary>
public static class ServiceCollectionExtensions
{
    /// <summary>Serilog 日志配置（须在 builder.Build() 之前调用）</summary>
    public static void AddInkWordLogging(this WebApplicationBuilder builder)
    {
        Log.Logger = new LoggerConfiguration()
            .ReadFrom.Configuration(builder.Configuration)
            .Enrich.FromLogContext()
            .WriteTo.Console()
            .WriteTo.File("logs/inkword-.log", rollingInterval: RollingInterval.Day)
            .CreateLogger();
        builder.Host.UseSerilog();
    }

    /// <summary>EF Core (PostgreSQL) + Redis</summary>
    public static IServiceCollection AddInkWordData(this IServiceCollection services,
        IConfiguration configuration)
    {
        // EF Core
        services.AddDbContext<AppDbContext>(opt =>
            opt.UseNpgsql(configuration.GetConnectionString("Postgres")));

        // Redis
        var redisConn = configuration.GetConnectionString("Redis") ?? "localhost:6379";
        services.AddSingleton<IConnectionMultiplexer>(_ => ConnectionMultiplexer.Connect(redisConn));
        services.AddStackExchangeRedisCache(opt => opt.Configuration = redisConn);
        services.AddSingleton<IRedisCache, RedisCache>();

        return services;
    }

    /// <summary>仓储 + 工作单元</summary>
    public static IServiceCollection AddInkWordRepositories(this IServiceCollection services)
    {
        // 仓储
        services.AddScoped<IWordRepository, WordRepository>();
        services.AddScoped<IDeviceRepository, DeviceRepository>();
        services.AddScoped<IUserRepository, UserRepository>();
        services.AddScoped<IAccountRepository, AccountRepository>();
        services.AddScoped<ILearningRecordRepository, LearningRecordRepository>();
        services.AddScoped<IOtaPackageRepository, OtaPackageRepository>();
        services.AddScoped<IBookRepository, BookRepository>();
        services.AddScoped<IReadingProgressRepository, ReadingProgressRepository>();
        services.AddScoped<IDeviceBookmarkRepository, DeviceBookmarkRepository>();

        // 工作单元（统一 SaveChanges + 复杂只读查询入口）
        services.AddScoped<IUnitOfWork, AppUnitOfWork>();

        return services;
    }

    /// <summary>业务服务（SRS/AI/TTS/ASR/Chat/VoiceSearch/Book）</summary>
    public static IServiceCollection AddInkWordServices(this IServiceCollection services,
        IConfiguration configuration)
    {
        services.AddScoped<SrsService>();
        services.AddScoped<FsrsService>();
        services.AddScoped<AiContentService>();
        services.AddScoped<PronunciationService>();
        services.AddScoped<TtsService>();
        services.AddScoped<ChatService>();
        services.AddScoped<VoiceSearchService>();
        services.AddScoped<BookService>();

        // ASR 转写引擎
        services.AddSingleton<IAsrTranscriber, SherpaAsrService>();

        // TTS 合成引擎（当前仅 piper；cloud 预留）
        if (string.Equals(configuration["Tts:Provider"], "cloud",
            StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException("Tts:Provider=cloud 尚未接入，请使用 piper");
        }
        services.AddSingleton<ISpeechSynthesizer, PiperSynthesizer>();

        // AI IChatClient（按 Ai:Provider 装配：本地 Ollama / OpenAI 兼容云 API）
        services.AddInkWordAiClient(configuration);

        // Polly 重试策略：AI 外部调用 resilience（指数退避 3 次重试）
        services.AddSingleton(new ResiliencePipelineBuilder()
            .AddRetry(new RetryStrategyOptions
            {
                MaxRetryAttempts = 3,
                BackoffType = DelayBackoffType.Exponential,
                Delay = TimeSpan.FromSeconds(2),
                OnRetry = args =>
                {
                    Log.Warning("AI 调用重试 {Attempt}/{Max}：{Reason}",
                        args.AttemptNumber + 1, 3, args.Outcome.Exception?.Message);
                    return default;
                }
            })
            .Build());

        return services;
    }

    /// <summary>AI IChatClient 装配（Ollama 默认 / OpenAI 兼容云 API）</summary>
    private static IServiceCollection AddInkWordAiClient(this IServiceCollection services,
        IConfiguration configuration)
    {
        var aiCfg = configuration.GetSection("Ai");
        var aiModel = aiCfg["Model"] ?? "qwen2.5:7b";

        if (string.Equals(aiCfg["Provider"], "openai", StringComparison.OrdinalIgnoreCase))
        {
            var apiKey = aiCfg["CloudApiKey"];
            if (string.IsNullOrEmpty(apiKey))
                throw new InvalidOperationException("Ai:Provider=openai 需配置 Ai:CloudApiKey");
            var endpoint = aiCfg["CloudEndpoint"];
            var oaClient = string.IsNullOrEmpty(endpoint)
                ? new OpenAIClient(apiKey)
                : new OpenAIClient(new ApiKeyCredential(apiKey),
                    new OpenAIClientOptions { Endpoint = new Uri(endpoint) });
            services.AddSingleton<IChatClient>(_ => oaClient.GetChatClient(aiModel).AsIChatClient());
        }
        else
        {
            // localhost 出站禁走代理（防系统代理干扰 Ollama 连接）
            services.AddSingleton<IChatClient>(_ => new OllamaChatClient(
                new Uri(aiCfg["OllamaUrl"] ?? "http://localhost:11434"), aiModel,
                new HttpClient(new SocketsHttpHandler { UseProxy = false }, disposeHandler: true)));
        }
        return services;
    }

    /// <summary>Hangfire 任务类注册</summary>
    public static IServiceCollection AddInkWordJobs(this IServiceCollection services)
    {
        services.AddTransient<DailyPushJob>();
        services.AddTransient<CleanupJob>();
        services.AddTransient<AiContentJob>();
        services.AddTransient<TtsJob>();
        services.AddTransient<ChatAudioCleanupJob>();
        services.AddTransient<ChatTurnLogger>();
        services.AddTransient<ChatTurnCleanupJob>();
        services.AddTransient<ChatReviewJob>();
        services.AddSingleton<IChatTurnSink, HangfireChatTurnSink>();
        services.AddSingleton<IWordListProvider, CachedWordListProvider>();
        return services;
    }

    /// <summary>认证（JWT）+ 授权 + CORS + Swagger + 健康检查</summary>
    public static IServiceCollection AddInkWordApi(this IServiceCollection services,
        IConfiguration configuration)
    {
        // 设备认证过滤器
        services.AddScoped<DeviceAuthFilter>();

        // JWT
        var jwtCfg = configuration.GetSection("Jwt");
        services.AddAuthentication(JwtBearerDefaults.AuthenticationScheme)
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
                    IssuerSigningKey = new SymmetricSecurityKey(
                        Encoding.UTF8.GetBytes(jwtCfg["Secret"]!))
                };
            });

        services.AddControllers();
        services.AddEndpointsApiExplorer();

        // Swagger（带 JWT 锁头按钮）
        services.AddSwaggerGen(c =>
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
                        Reference = new OpenApiReference
                        {
                            Type = ReferenceType.SecurityScheme, Id = "Bearer"
                        }
                    }, Array.Empty<string>()
                }
            });
        });

        // CORS（供 Angular 前端）
        services.AddCors(opt => opt.AddDefaultPolicy(p =>
            p.AllowAnyOrigin().AllowAnyHeader().AllowAnyMethod()));

        // Hangfire
        services.AddHangfire(config => config
            .UseSimpleAssemblyNameTypeSerializer()
            .UseRecommendedSerializerSettings()
            .UsePostgreSqlStorage(c => c.UseNpgsqlConnection(
                configuration.GetConnectionString("Postgres"))));
        services.AddHangfireServer();

        // 健康检查（Docker 部署 / 负载均衡探针）
        services.AddHealthChecks()
            .AddDbContextCheck<AppDbContext>();

        // API 版本控制（URL 段 /api/v1/...；默认 v1 兼容现有客户端）
        services.AddApiVersioning(opt =>
        {
            opt.DefaultApiVersion = new ApiVersion(1, 0);
            opt.AssumeDefaultVersionWhenUnspecified = true;
            opt.ApiVersionReader = ApiVersionReader.Combine(
                new UrlSegmentApiVersionReader(),
                new QueryStringApiVersionReader("api-version"));
        }).AddApiExplorer(opt =>
        {
            opt.GroupNameFormat = "'v'VVV";
            opt.SubstituteApiVersionInUrl = true;
        });

        return services;
    }
}
