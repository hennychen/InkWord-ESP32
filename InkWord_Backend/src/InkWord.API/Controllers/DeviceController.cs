using System.Security.Cryptography;
using InkWord.API.DTOs;
using InkWord.API.Filters;
using InkWord.Core.Common;
using InkWord.Core.Entities;
using InkWord.Core.Repositories;
using InkWord.Services;
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Mvc;

namespace InkWord.API.Controllers;

/// <summary>
/// 设备端接口（供 ESP32 调用）。
/// </summary>
[ApiController]
[Route("api/device")]
public class DeviceController : ControllerBase
{
    private readonly IDeviceRepository _deviceRepo;
    private readonly IWordRepository _wordRepo;
    private readonly ILearningRecordRepository _recordRepo;
    private readonly IOtaPackageRepository _otaRepo;
    private readonly SrsService _srs;

    public DeviceController(IDeviceRepository deviceRepo, IWordRepository wordRepo,
        ILearningRecordRepository recordRepo, IOtaPackageRepository otaRepo, SrsService srs)
    {
        _deviceRepo = deviceRepo; _wordRepo = wordRepo;
        _recordRepo = recordRepo; _otaRepo = otaRepo; _srs = srs;
    }

    /// <summary>B-08 首次注册：生成 ApiKey</summary>
    [HttpPost("register")]
    [AllowAnonymous]
    public async Task<IActionResult> Register([FromBody] RegisterReq req, CancellationToken ct)
    {
        if (string.IsNullOrWhiteSpace(req.Mac))
            return BadRequest(ApiResponse.Fail(400, "mac required"));

        // 已注册则返回旧记录的 ApiKey
        var existing = await _deviceRepo.GetByMacAsync(req.Mac, ct);
        if (existing != null)
            return Ok(ApiResponse<RegisterResp>.Ok(new RegisterResp(existing.Id, existing.ApiKey)));

        var device = new Device
        {
            MacAddress = req.Mac,
            Name = req.Name ?? $"InkWord-{req.Mac[^4..]}",
            ApiKey = GenerateApiKey(),
            LastHeartbeat = DateTime.UtcNow
        };
        await _deviceRepo.AddAsync(device, ct);
        await _deviceRepo.SaveChangesAsync(ct);

        return Ok(ApiResponse<RegisterResp>.Ok(new RegisterResp(device.Id, device.ApiKey)));
    }

    /// <summary>B-09 增量拉取词库</summary>
    [HttpGet("sync/words")]
    [ServiceFilter(typeof(DeviceAuthFilter))]
    public async Task<IActionResult> SyncWords([FromQuery] int version, [FromQuery] int count, CancellationToken ct)
    {
        count = count <= 0 || count > 2000 ? 500 : count;
        var device = (Device)HttpContext.Items["Device"]!;

        var words = await _wordRepo.GetIncrementalAsync(version, count, ct);
        var newVersion = await _wordRepo.GetMaxVersionAsync(ct);

        // 顺手更新设备本地版本
        device.WordVersion = newVersion;
        await _deviceRepo.SaveChangesAsync(ct);

        var dto = new SyncResp(newVersion, words.Select(w => new WordDto(
            w.Text, w.Phonetic, w.Meaning, w.Example, w.Audio, w.Tag,
            w.Difficulty, w.Version, w.ChangeType)).ToList());

        Response.Headers["X-Word-Version"] = newVersion.ToString();
        return Ok(ApiResponse<SyncResp>.Ok(dto));
    }

    /// <summary>B-10 批量回传学习记录</summary>
    [HttpPost("sync/progress")]
    [ServiceFilter(typeof(DeviceAuthFilter))]
    public async Task<IActionResult> SyncProgress([FromBody] List<ProgressItem> items, CancellationToken ct)
    {
        var device = (Device)HttpContext.Items["Device"]!;
        var now = DateTime.UtcNow;

        foreach (var item in items)
        {
            var rec = await _recordRepo.GetAsync(device.Id, item.WordId, ct);
            if (rec == null)
            {
                rec = new LearningRecord { DeviceId = device.Id, WordId = item.WordId };
                _srs.InitRecord(rec, now);
                await _recordRepo.AddAsync(rec, ct);
            }
            _srs.ApplyReview(rec, item.Quality,
                item.Timestamp > 0 ? DateTimeOffset.FromUnixTimeSeconds(item.Timestamp).UtcDateTime : now);
        }
        await _recordRepo.SaveChangesAsync(ct);

        return Ok(ApiResponse.Ok());
    }

    /// <summary>B-10b 收藏上报（设备端 SET 长按切换后同步）</summary>
    [HttpPost("sync/collect")]
    [ServiceFilter(typeof(DeviceAuthFilter))]
    public async Task<IActionResult> SyncCollect([FromBody] CollectReq req, CancellationToken ct)
    {
        var device = (Device)HttpContext.Items["Device"]!;
        var rec = await _recordRepo.GetAsync(device.Id, req.WordId, ct);
        if (rec == null)
        {
            // 未学过的词直接收藏：落一条初始记录
            rec = new LearningRecord { DeviceId = device.Id, WordId = req.WordId };
            _srs.InitRecord(rec, DateTime.UtcNow);
            await _recordRepo.AddAsync(rec, ct);
        }
        rec.IsCollected = req.Collected;
        await _recordRepo.SaveChangesAsync(ct);

        return Ok(ApiResponse.Ok());
    }

    /// <summary>B-11 心跳</summary>
    [HttpPost("heartbeat")]
    [ServiceFilter(typeof(DeviceAuthFilter))]
    public async Task<IActionResult> Heartbeat([FromBody] HeartbeatReq req, CancellationToken ct)
    {
        var device = (Device)HttpContext.Items["Device"]!;
        device.LastHeartbeat = DateTime.UtcNow;
        device.BatteryLevel = req.Battery;
        if (!string.IsNullOrEmpty(req.Version)) device.FirmwareVersion = req.Version;
        await _deviceRepo.SaveChangesAsync(ct);
        return Ok(ApiResponse.Ok());
    }

    /// <summary>B-12 OTA 检查</summary>
    [HttpGet("ota/check")]
    [AllowAnonymous]   // 也可加设备认证，这里放宽以便首次升级
    public async Task<IActionResult> OtaCheck([FromQuery] string currentVer, CancellationToken ct)
    {
        var latest = await _otaRepo.GetLatestPublishedAsync("esp32-s3", ct);
        if (latest == null || !IsNewer(latest.Version, currentVer))
            return Ok(ApiResponse<OtaCheckResp>.Ok(new OtaCheckResp(false, "", "", 0, "")));

        return Ok(ApiResponse<OtaCheckResp>.Ok(new OtaCheckResp(
            true, latest.Url, latest.Md5, (int)latest.Size, latest.Version)));
    }

    // ---- helpers ----
    private static string GenerateApiKey()
    {
        var bytes = RandomNumberGenerator.GetBytes(24);
        return Convert.ToHexString(bytes).ToLowerInvariant();  // 48 位十六进制
    }

    private static bool IsNewer(string latest, string current)
    {
        if (Version.TryParse(latest, out var l) && Version.TryParse(current, out var c))
            return l > c;
        return !string.Equals(latest, current, StringComparison.OrdinalIgnoreCase);
    }
}
