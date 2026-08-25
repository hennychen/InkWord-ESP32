using InkWord.API.DTOs;
using InkWord.Core.Common;
using InkWord.Core.Repositories;
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Mvc;
using StackExchange.Redis;

namespace InkWord.API.Controllers;

/// <summary>设备管理与远程指令（B-16）。</summary>
[ApiController]
[Route("api/admin/devices")]
[Authorize(Roles = "Admin,Operator")]
public class AdminDeviceController : ControllerBase
{
    private readonly IDeviceRepository _deviceRepo;
    private readonly IConnectionMultiplexer _redis;

    public AdminDeviceController(IDeviceRepository deviceRepo, IConnectionMultiplexer redis)
    {
        _deviceRepo = deviceRepo;
        _redis = redis;
    }

    [HttpGet]
    public async Task<IActionResult> List([FromQuery] int page = 1, [FromQuery] int size = 20, CancellationToken ct = default)
    {
        var (items, total) = await _deviceRepo.GetPagedAsync(page, size, null, ct);
        return Ok(ApiResponse<PagedResult<Core.Entities.Device>>.Ok(
            new PagedResult<Core.Entities.Device>(items, total, page, size)));
    }

    [HttpGet("{id:guid}")]
    public async Task<IActionResult> Detail(Guid id, CancellationToken ct)
    {
        var d = await _deviceRepo.GetByIdAsync(id, ct);
        if (d == null) return NotFound(ApiResponse.Fail(404, "not found"));
        return Ok(ApiResponse<Core.Entities.Device>.Ok(d));
    }

    /// <summary>下发远程指令（强制同步/全刷/OTA），通过 Redis Pub/Sub 推送到设备。</summary>
    [HttpPost("{id:guid}/command")]
    public async Task<IActionResult> SendCommand(Guid id, [FromBody] CommandReq req, CancellationToken ct)
    {
        var d = await _deviceRepo.GetByIdAsync(id, ct);
        if (d == null) return NotFound(ApiResponse.Fail(404, "not found"));

        var channel = $"device:cmd:{d.ApiKey[..8]}";
        var pub = _redis.GetSubscriber();
        await pub.PublishAsync(RedisChannel.Literal(channel), req.Action);

        return Ok(ApiResponse.Ok($"指令 '{req.Action}' 已下发至 {d.Name}"));
    }
}
