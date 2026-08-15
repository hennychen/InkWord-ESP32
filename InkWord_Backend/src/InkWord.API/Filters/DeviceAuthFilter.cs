using InkWord.Core.Repositories;
using Microsoft.AspNetCore.Mvc;
using Microsoft.AspNetCore.Mvc.Filters;

namespace InkWord.API.Filters;

/// <summary>
/// 设备认证过滤器 (Task B-07)：校验请求头 X-Device-Key 是否匹配数据库设备。
/// 标记 [ServiceFilter(typeof(DeviceAuthFilter))] 的 Action 会经过校验，
/// 认证通过后把 Device 写入 HttpContext.Items["Device"]。
/// </summary>
public class DeviceAuthFilter : IAsyncActionFilter
{
    private readonly IDeviceRepository _deviceRepo;
    public DeviceAuthFilter(IDeviceRepository deviceRepo) => _deviceRepo = deviceRepo;

    public async Task OnActionExecutionAsync(ActionExecutingContext context, ActionExecutionDelegate next)
    {
        var headers = context.HttpContext.Request.Headers;
        if (!headers.TryGetValue("X-Device-Key", out var key) || string.IsNullOrWhiteSpace(key))
        {
            context.Result = new UnauthorizedObjectResult(new { code = 401, message = "missing X-Device-Key" });
            return;
        }

        var device = await _deviceRepo.GetByApiKeyAsync(key.ToString(), context.HttpContext.RequestAborted);
        if (device == null)
        {
            context.Result = new UnauthorizedObjectResult(new { code = 401, message = "invalid device key" });
            return;
        }

        context.HttpContext.Items["Device"] = device;
        await next();
    }
}
