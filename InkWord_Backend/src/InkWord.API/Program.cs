using Hangfire;
using InkWord.API.Extensions;
using Serilog;

var builder = WebApplication.CreateBuilder(args);

// ---- 日志（须在 Build 前配置） ----
builder.AddInkWordLogging();

// ---- 数据层（EF Core + Redis） ----
builder.Services.AddInkWordData(builder.Configuration);

// ---- 仓储 + 工作单元 ----
builder.Services.AddInkWordRepositories();

// ---- 业务服务（SRS/AI/TTS/ASR/Chat/VoiceSearch/Book） ----
builder.Services.AddInkWordServices(builder.Configuration);

// ---- Hangfire 任务类 ----
builder.Services.AddInkWordJobs();

// ---- API 层（JWT/Swagger/CORS/Hangfire/健康检查） ----
builder.Services.AddInkWordApi(builder.Configuration);

var app = builder.Build();

// ---- 全局异常中间件（最先注册） ----
app.UseMiddleware<InkWord.API.Middlewares.ExceptionMiddleware>();

app.UseSwagger();
app.UseSwaggerUI();
app.UseCors();
app.UseAuthentication();
app.UseAuthorization();

// ---- 数据库迁移（开发环境自动建表 + 幂等补丁） ----
if (app.Environment.IsDevelopment())
{
    app.MigrateInkWordDatabase();
}

// ---- 种子数据（词库 + 古诗文） ----
await app.SeedInkWordDataAsync();

// ---- Hangfire Dashboard ----
app.UseHangfireDashboard("/hangfire");
InkWord.Jobs.JobRegistrar.Register();

// ---- 健康检查端点 ----
app.MapHealthChecks("/health");

app.MapControllers();

app.Run();
