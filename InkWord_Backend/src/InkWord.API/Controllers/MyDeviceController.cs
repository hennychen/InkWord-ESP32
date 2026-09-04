using System.Security.Claims;
using InkWord.Core.Common;
using InkWord.Infrastructure.DbContext;
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Mvc;
using Microsoft.EntityFrameworkCore;

namespace InkWord.API.Controllers;

/// <summary>
/// 学习者设备管理与跨设备聚合（v2.0 完整账户，ACCOUNT_MODEL_DECISION §五）。
///
/// 绑定：App LAN 发现设备（/api/stats 的 mac 字段）→ POST devices/bind
/// {mac} → 写 Device.UserId + ApiKey 换发。换发后旧钥即刻失效，设备下个
/// 周期 401 → 清 key 重注册（MAC 幂等取回新钥，固件 sync_client 自愈
/// 微调）；新钥不回传 App（无写回通道，自愈链路已闭环）。解绑：UserId
/// 置空，ApiKey 不换发（设备回无主状态零感知）。学习记录仍挂 DeviceId
/// （红线 §三.2），多设备同步 = LWS 聚合只读视图（LastStudiedAt 新者胜，
/// 整条记录取自胜者设备——FSRS 状态不可拆分合并），深度进度漫游为
/// v2.x+ 增强不在 v2.0 承诺。
/// </summary>
[ApiController]
[Route("api/me")]
[Authorize(Roles = "learner")]
public class MyDeviceController : ControllerBase
{
    private readonly AppDbContext _db;

    public MyDeviceController(AppDbContext db) => _db = db;

    public record BindReq(string Mac);
    public record DeviceDto(Guid Id, string Name, string Mac, string FirmwareVersion,
        int BatteryLevel, bool Online, DateTime LastHeartbeat, int RecordCount);
    public record AggregateDto(Guid WordId, Guid DeviceId, double Stability,
        double Difficulty, DateTime? NextReview, bool IsCollected, bool IsMastered,
        DateTime LastStudiedAt);

    /// <summary>LWS 归并行（查询投影形态；sealed record 供归并直测）</summary>
    public sealed record LwsRow(Guid WordId, Guid DeviceId, double Stability,
        double Difficulty, DateTime? NextReview, bool IsCollected, bool IsMastered,
        DateTime LastStudiedAt);

    /// <summary>我的设备清单（电量/在线/学习记录数）</summary>
    [HttpGet("devices")]
    public async Task<IActionResult> List(CancellationToken ct)
    {
        var accountId = AccountId;
        var devices = await _db.Devices.AsNoTracking()
            .Where(d => d.UserId == accountId)
            .OrderByDescending(d => d.LastHeartbeat)
            .ToListAsync(ct);
        if (devices.Count == 0)
            return Ok(ApiResponse<List<DeviceDto>>.Ok([]));

        var ids = devices.Select(d => d.Id).ToList();
        var counts = await _db.LearningRecords.AsNoTracking()
            .Where(lr => ids.Contains(lr.DeviceId))
            .GroupBy(lr => lr.DeviceId)
            .ToDictionaryAsync(g => g.Key, g => g.Count(), ct);

        var dto = devices.Select(d => new DeviceDto(
            d.Id, d.Name, d.MacAddress, d.FirmwareVersion,
            d.BatteryLevel, d.IsOnline, d.LastHeartbeat,
            counts.GetValueOrDefault(d.Id, 0))).ToList();
        return Ok(ApiResponse<List<DeviceDto>>.Ok(dto));
    }

    /// <summary>绑定设备（凭 MAC——App 经 LAN /api/stats 获取）。
    /// 幂等：已绑本账户直接成功不重复换发（防 App 重试风暴把设备踢下线）；
    /// 绑定他人账户的设备返回 409。</summary>
    [HttpPost("devices/bind")]
    public async Task<IActionResult> Bind([FromBody] BindReq req, CancellationToken ct)
    {
        var mac = (req.Mac ?? "").Trim().ToUpperInvariant();
        if (mac.Length is < 12 or > 32)
            return BadRequest(ApiResponse.Fail(400, "mac 无效（12 位十六进制）"));

        var device = await _db.Devices.FirstOrDefaultAsync(d => d.MacAddress == mac, ct);
        if (device == null)
            return NotFound(ApiResponse.Fail(404, "设备未注册（请先让设备联网完成注册）"));

        var accountId = AccountId;
        if (device.UserId != null && device.UserId != accountId)
            return Conflict(ApiResponse.Fail(409, "设备已绑定其他账户"));

        if (device.UserId != accountId)
        {
            device.UserId = accountId;
            // 换发：旧钥即刻失效（防旧绑定方残留控制），设备 401 自愈重注册
            device.ApiKey = DeviceController.GenerateApiKey();
            await _db.SaveChangesAsync(ct);
        }

        return Ok(ApiResponse<DeviceDto>.Ok(new DeviceDto(
            device.Id, device.Name, device.MacAddress, device.FirmwareVersion,
            device.BatteryLevel, device.IsOnline, device.LastHeartbeat, 0)));
    }

    /// <summary>解绑设备（UserId 置空；ApiKey 不换发——设备端零感知）</summary>
    [HttpPost("devices/{id}/unbind")]
    public async Task<IActionResult> Unbind(Guid id, CancellationToken ct)
    {
        var device = await _db.Devices.FirstOrDefaultAsync(d => d.Id == id, ct);
        if (device == null || device.UserId != AccountId)
            return NotFound(ApiResponse.Fail(404, "设备不在你的账户"));

        device.UserId = null;
        await _db.SaveChangesAsync(ct);
        return Ok(ApiResponse.Ok());
    }

    /// <summary>跨设备 LWS 聚合视图（只读）。take 默认 500 上限 2000；
    /// 投影行按设备数放大预取后内存归并（EF 整行比较聚合不可翻译，
    /// 投影先行同 today-stats 先例）。</summary>
    [HttpGet("progress/aggregate")]
    public async Task<IActionResult> Aggregate([FromQuery] int take, CancellationToken ct)
    {
        take = take <= 0 || take > 2000 ? 500 : take;

        var accountId = AccountId;
        var deviceIds = await _db.Devices.AsNoTracking()
            .Where(d => d.UserId == accountId)
            .Select(d => d.Id)
            .ToListAsync(ct);
        if (deviceIds.Count == 0)
            return Ok(ApiResponse<List<AggregateDto>>.Ok([]));

        var rows = await _db.LearningRecords.AsNoTracking()
            .Where(lr => deviceIds.Contains(lr.DeviceId))
            .OrderByDescending(lr => lr.LastStudiedAt)
            .Select(lr => new LwsRow(lr.WordId, lr.DeviceId, lr.FsrsStability,
                lr.FsrsDifficulty, lr.FsrsNextReview, lr.IsCollected, lr.IsMastered,
                lr.LastStudiedAt))
            .Take(take * deviceIds.Count)
            .ToListAsync(ct);

        var dto = LwsMerge(rows, take)
            .Select(r => new AggregateDto(r.WordId, r.DeviceId, r.Stability,
                r.Difficulty, r.NextReview, r.IsCollected, r.IsMastered, r.LastStudiedAt))
            .ToList();
        return Ok(ApiResponse<List<AggregateDto>>.Ok(dto));
    }

    // ---- 内部 ----

    /// <summary>LWS 归并：按 WordId 取 LastStudiedAt 新者整行胜出，
    /// 结果按最近学习降序截 take（public 供测试直测）。</summary>
    public static List<LwsRow> LwsMerge(List<LwsRow> rows, int take)
    {
        // 输入已按 LastStudiedAt 降序（查询排序契约）；防御性再排一次，
        // 直测入口不保证有序
        var win = new Dictionary<Guid, LwsRow>();
        foreach (var r in rows.OrderByDescending(r => r.LastStudiedAt))
            if (!win.ContainsKey(r.WordId))
                win[r.WordId] = r;
        return win.Values.OrderByDescending(r => r.LastStudiedAt).Take(take).ToList();
    }

    private Guid AccountId =>
        Guid.TryParse(User.FindFirstValue(ClaimTypes.NameIdentifier), out var g)
            ? g
            : throw new UnauthorizedAccessException("token 缺少账户标识");
}
