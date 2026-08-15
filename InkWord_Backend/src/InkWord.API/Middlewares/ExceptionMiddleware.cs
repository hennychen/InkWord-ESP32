using System.Net;
using System.Text.Json;
using InkWord.Core.Common;

namespace InkWord.API.Middlewares;

/// <summary>
/// 全局异常处理中间件 (Task B-22)：
/// 捕获未处理异常，统一返回 ApiResponse 标准格式，集成 Serilog 写文件。
/// </summary>
public class ExceptionMiddleware
{
    private readonly RequestDelegate _next;
    private readonly ILogger<ExceptionMiddleware> _logger;
    private static readonly JsonSerializerOptions JsonOpts = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase };

    public ExceptionMiddleware(RequestDelegate next, ILogger<ExceptionMiddleware> logger)
    {
        _next = next;
        _logger = logger;
    }

    public async Task InvokeAsync(HttpContext context)
    {
        try
        {
            await _next(context);
        }
        catch (Exception ex)
        {
            _logger.LogError(ex, "未处理异常: {Path}", context.Request.Path);
            await WriteError(context, HttpStatusCode.InternalServerError, "服务器内部错误，请稍后重试");
        }
    }

    private static Task WriteError(HttpContext context, HttpStatusCode code, string message)
    {
        context.Response.ContentType = "application/json; charset=utf-8";
        context.Response.StatusCode = (int)code;

        var payload = ApiResponse.Fail((int)code, message);
        return context.Response.WriteAsync(JsonSerializer.Serialize(payload, JsonOpts));
    }
}
