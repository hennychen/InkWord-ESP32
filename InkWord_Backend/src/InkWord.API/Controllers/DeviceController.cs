using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using InkWord.API.DTOs;
using InkWord.API.Filters;
using InkWord.Core.Common;
using InkWord.Core.Entities;
using InkWord.Core.Repositories;
using InkWord.Infrastructure.Cache;
using InkWord.Infrastructure.DbContext;
using InkWord.Jobs;
using InkWord.Services;
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Mvc;
using Microsoft.EntityFrameworkCore;

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
    private readonly PronunciationService _pron;
    private readonly TtsService _tts;
    private readonly ChatService _chatSvc;
    private readonly VoiceSearchService _voice;
    private readonly AppDbContext _db; // T4.1：v2 归属映射（deck/subject 身份）
    private readonly IRedisCache _cache; // A3：chat-review 周报 Redis 热路径

    public DeviceController(IDeviceRepository deviceRepo, IWordRepository wordRepo,
        ILearningRecordRepository recordRepo, IOtaPackageRepository otaRepo,
        SrsService srs, PronunciationService pron, TtsService tts, ChatService chatSvc,
        VoiceSearchService voice, AppDbContext db, IRedisCache cache)
    {
        _deviceRepo = deviceRepo; _wordRepo = wordRepo;
        _recordRepo = recordRepo; _otaRepo = otaRepo; _srs = srs; _pron = pron;
        _tts = tts;
        _chatSvc = chatSvc;
        _voice = voice;
        _db = db;
        _cache = cache;
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

        // v2（T4.1 全科地基）双写：deck/subject 身份映射，与 export 端点同源。
        // 表行数极小（个位数），每请求全量拉取无压力。
        var decks = await _db.Decks.AsNoTracking()
            .Select(d => new { d.Id, d.Code, d.PayloadType, d.SubjectId }).ToListAsync(ct);
        var deckById = decks.ToDictionary(d => d.Id);
        var subCodes = await _db.Subjects.AsNoTracking()
            .ToDictionaryAsync(s => s.Id, s => s.Code, ct);

        var dto = new SyncResp(newVersion, words.Select(w =>
        {
            var deck = w.DeckId.HasValue && deckById.TryGetValue(w.DeckId.Value, out var d) ? d : null;
            var subject = w.SubjectId.HasValue && subCodes.TryGetValue(w.SubjectId.Value, out var sc)
                ? sc
                : deck != null ? subCodes.GetValueOrDefault(deck.SubjectId, "en") : "en";
            return new WordDto(
                w.Text, w.Phonetic, w.Meaning, w.Example, w.Audio, w.Tag,
                w.Difficulty, w.Version, w.ChangeType,
                subject,
                deck?.Code ?? "junior",
                deck?.PayloadType ?? "word-card",
                w.Front != "" ? w.Front : w.Text,
                w.Back != "" ? w.Back : w.Meaning,
                w.PayloadJson ?? "");
        }).ToList());

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

        // 批内同词多次评分（离线攒批回放）：本地缓存已加载记录。
        // 未缓存时首条尚未 SaveChanges，GetAsync 查库仍为 null，会重复
        // Insert 触发 IX_LearningRecords_DeviceId_WordId 唯一索引冲突（23505）。
        var batch = new Dictionary<Guid, LearningRecord>();
        foreach (var item in items)
        {
            if (!batch.TryGetValue(item.WordId, out var rec))
            {
                rec = await _recordRepo.GetAsync(device.Id, item.WordId, ct);
                if (rec == null)
                {
                    rec = new LearningRecord { DeviceId = device.Id, WordId = item.WordId };
                    _srs.InitRecord(rec, now);
                    await _recordRepo.AddAsync(rec, ct);
                }
                batch[item.WordId] = rec;
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

    /// <summary>B-10c 墨封上报（设备端 toggle_master 切换后同步，2026-09-04）。
    /// 置位时同步清 ConsecutiveWrong（声明式通过，与固件 learning_state
    /// toggle_master 同规则——双端连错清零不漂移）；FSRS 调度状态原样保留
    /// （Anki suspend 哲学：启封无损回队）。</summary>
    [HttpPost("sync/master")]
    [ServiceFilter(typeof(DeviceAuthFilter))]
    public async Task<IActionResult> SyncMaster([FromBody] MasterReq req, CancellationToken ct)
    {
        var device = (Device)HttpContext.Items["Device"]!;
        var rec = await _recordRepo.GetAsync(device.Id, req.WordId, ct);
        if (rec == null)
        {
            // 未学过的词直接墨封：落一条初始记录（收藏上报同款先例）
            rec = new LearningRecord { DeviceId = device.Id, WordId = req.WordId };
            _srs.InitRecord(rec, DateTime.UtcNow);
            await _recordRepo.AddAsync(rec, ct);
        }
        rec.IsMastered = req.Mastered;
        if (req.Mastered) rec.ConsecutiveWrong = 0;
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

    /// <summary>M5 发音评测：multipart WAV（16kHz/16bit/mono ≤3s）+ 目标词 Guid。</summary>
    /// <remarks>返回 total 0~100（≥60 视为通过，设备端两短震/一长震）；
    /// engine=heuristic 为过渡引擎（GOP 集成前不具备音素级辨析，见
    /// docs/AI_SPEECH_ASSESSMENT.md）。评分写入 LastPronScore。</remarks>
    [HttpPost("pronunciation")]
    [ServiceFilter(typeof(DeviceAuthFilter))]
    public async Task<IActionResult> Pronunciation(
        [FromQuery] Guid wordId, IFormFile file, CancellationToken ct)
    {
        var device = (Device)HttpContext.Items["Device"]!;
        if (file == null || file.Length == 0)
            return BadRequest(ApiResponse.Fail(400, "empty file"));
        if (file.Length > 128 * 1024)
            return BadRequest(ApiResponse.Fail(400, "file too large"));

        var word = await _wordRepo.GetByIdAsync(wordId, ct);
        if (word == null) return NotFound(ApiResponse.Fail(404, "word not found"));

        using var ms = new MemoryStream();
        await file.CopyToAsync(ms, ct);

        PronunciationScore score;
        try
        {
            score = await _pron.AssessAsync(ms.ToArray(), word.Text, ct);
        }
        catch (InvalidDataException ex)
        {
            return BadRequest(ApiResponse.Fail(400, ex.Message));
        }

        // 落 LastPronScore（无学习记录则顺带建档）
        var rec = await _recordRepo.GetAsync(device.Id, wordId, ct);
        if (rec == null)
        {
            rec = new LearningRecord { DeviceId = device.Id, WordId = wordId };
            _srs.InitRecord(rec, DateTime.UtcNow);
            await _recordRepo.AddAsync(rec, ct);
        }
        rec.LastPronScore = score.Total;
        await _recordRepo.SaveChangesAsync(ct);

        return Ok(ApiResponse<PronunciationScore>.Ok(score));
    }

    /// <summary>P0B 音频下发：GET /api/device/audio/{file}。</summary>
    /// <remarks>词条音频 {wordId:N}.mp3（TtsJob 批量合成）与对话音频
    /// chat_*.mp3（P2A）共用本端点；文件名白名单校验（拒路径穿越），
    /// PhysicalFile 流式返回 audio/mpeg。设备端 audio_sync 按 words.json
    /// 的 cloudId 推导文件名拉取。</remarks>
    [HttpGet("audio/{file}")]
    [ServiceFilter(typeof(DeviceAuthFilter))]
    public IActionResult Audio(string file)
    {
        if (!_tts.TryResolveSafePath(file, out var fullPath) || !System.IO.File.Exists(fullPath))
            return NotFound(ApiResponse.Fail(404, "audio not found"));
        return PhysicalFile(fullPath, "audio/mpeg");
    }

    /// <summary>P2A 语音对话：multipart WAV（16kHz/16bit/mono ≤10s）→ ASR+LLM+TTS 单端点闭环。</summary>
    /// <remarks>A1 模式扩展（2026-08-28）：?mode={free|scenario|translate}&amp;scenarioId={code}
    /// （老固件不带 query = free，行为与现状一致）；scenario 模式响应
    /// data 额外携带 warmup（首轮中文预热，其余轮 null 可缺省）。
    /// 响应 { transcript, reply, engine, audioUrl, mode?, warmup? }；
    /// audioUrl=null 表示 TTS 失败（文本仍可用；translate 模式 LLM
    /// 未按两行格式时同样降级纯屏显）。错误：非 WAV/无话音/非法
    /// mode/未知场景 400、ASR 未配置 503、LLM 故障 502、超限 413。
    /// 会话上下文 Redis 按模式隔离（free=chat:{deviceId} 现状不变，
    /// scenario/translate 追加后缀），TTL 30 分钟；回复 MP3 落
    /// data/audio/chat_*.mp3（ChatAudioCleanupJob 每小时回收超 1 小时文件）。
    /// P0-1 流式（2026-08-30）：?stream=1 协商 NDJSON 逐行下发（meta→
    /// s×N→end，协议见 AI_CHAT_MODE.md §2b）；不带 stream 的老固件
    /// 逐字节走原 JSON 信封路径。流式错误：首行前异常走传统状态码
    /// 错误信封（Response 未开始）；流中途 LLM 失败发 err 行。</remarks>
    [HttpPost("chat")]
    [ServiceFilter(typeof(DeviceAuthFilter))]
    public async Task<IActionResult> Chat([FromQuery] string? mode,
        [FromQuery] string? scenarioId, [FromQuery] string? stream,
        IFormFile file, CancellationToken ct)
    {
        var device = (Device)HttpContext.Items["Device"]!;
        if (file == null || file.Length == 0)
            return BadRequest(ApiResponse.Fail(400, "empty file"));
        if (file.Length > ChatService.MaxWavBytes)
            return StatusCode(413, ApiResponse.Fail(413, "file too large"));

        using var ms = new MemoryStream();
        await file.CopyToAsync(ms, ct);

        // P0-1 流式分支：NDJSON 逐行写出（老后端兼容由固件端回退兑住，
        // 本端不带 stream=1 的行为与现状逐字节一致）
        if (stream == "1")
            return await ChatStream(device.Id, ms.ToArray(), mode, scenarioId, ct);

        ChatReply reply;
        try
        {
            reply = await _chatSvc.ConverseAsync(device.Id, ms.ToArray(), mode, scenarioId, ct);
        }
        catch (ChatException ex)
        {
            return StatusCode(ex.StatusCode, ApiResponse.Fail(ex.StatusCode, ex.Message));
        }

        return Ok(ApiResponse<ChatReply>.Ok(reply));
    }

    /// <summary>P0-1 流式写出：application/x-ndjson + X-Accel-Buffering:no
    /// （反代禁缓冲，逐行到达）；行由 ConverseStreamAsync 序列化，
    /// 此处只补 \n + Flush。首行前 ChatException（400/503）时响应未
    /// 开始，走传统状态码错误信封（固件统一按 status!=200 处置）。</summary>
    private async Task<IActionResult> ChatStream(Guid deviceId, byte[] wav,
        string? mode, string? scenarioId, CancellationToken ct)
    {
        Response.StatusCode = 200;
        Response.ContentType = "application/x-ndjson";
        Response.Headers["X-Accel-Buffering"] = "no";
        try
        {
            await _chatSvc.ConverseStreamAsync(deviceId, wav, mode, scenarioId,
                async line =>
                {
                    await Response.Body.WriteAsync(Encoding.UTF8.GetBytes(line + "\n"), ct);
                    await Response.Body.FlushAsync(ct);
                }, ct);
        }
        catch (ChatException ex) when (!Response.HasStarted)
        {
            // 首行前异常：ContentType 已被设为 x-ndjson，不复位则错误信封
            // 按 ndjson 协商 formatter 失败被吞成 406（curl 实测发现）
            Response.ContentType = "application/json";
            return StatusCode(ex.StatusCode, ApiResponse.Fail(ex.StatusCode, ex.Message));
        }
        catch (ChatException ex)   // 防御：理论上不可达（流内异常已转 err 行）
        {
            await Response.Body.WriteAsync(Encoding.UTF8.GetBytes(
                System.Text.Json.JsonSerializer.Serialize(
                    new { t = "err", code = ex.StatusCode, m = ex.Message }) + "\n"), ct);
            await Response.Body.FlushAsync(ct);
        }
        return new EmptyResult();
    }

    /// <summary>P0-2 设备打断上报：fire-and-forget 截断在途轮次。</summary>
    /// <remarks>设备播放中/等待中打断时携带 meta 行下发的 roundId 调用；
    /// 未知/已结束 roundId 返回 200 no-op（晚到/早退均安全）。单实例
    /// 内存注册表（多实例部署约束见 AI_CHAT_MODE.md §2b）。</remarks>
    [HttpPost("chat/abort")]
    [ServiceFilter(typeof(DeviceAuthFilter))]
    public IActionResult ChatAbort([FromQuery] Guid? roundId)
    {
        if (roundId is { } id)
            _chatSvc.AbortRound(id);
        return Ok(ApiResponse.Ok());
    }

    /// <summary>语音查词：multipart WAV（16kHz/16bit/mono ≤5s）→ ASR → 词库三级匹配。</summary>
    /// <remarks>?deck={Code} 限定设备活跃词书范围（失配/缺省兜底全库）；
    /// 响应 { transcript, candidates:[{text,meaning,cloudId,score}] }（top-5）。
    /// 错误：非 WAV/无话音 400、ASR 未配置 503、超限 413（chat 同款）。</remarks>
[HttpPost("voice-search")]
    [ServiceFilter(typeof(DeviceAuthFilter))]
public async Task<IActionResult> VoiceSearch(
        [FromQuery] string? deck, IFormFile file, CancellationToken ct)
    {
        if (file == null || file.Length == 0)
            return BadRequest(ApiResponse.Fail(400, "empty file"));
        if (file.Length > VoiceSearchService.MaxWavBytes)
            return StatusCode(413, ApiResponse.Fail(413, "file too large"));

        using var ms = new MemoryStream();
        await file.CopyToAsync(ms, ct);

        try
        {
            var result = await _voice.SearchAsync(ms.ToArray(), deck, ct);
            return Ok(ApiResponse<VoiceSearchResult>.Ok(result));
        }
        catch (VoiceSearchException ex)
        {
            return StatusCode(ex.StatusCode, ApiResponse.Fail(ex.StatusCode, ex.Message));
        }
    }

    // ---- helpers ----
    /// <summary>对话周报（A3）：GET /api/device/chat-review。</summary>
    /// <remarks>设备快捷菜单「对话周报」项拉取：Redis 热路径（周报 Job
    /// 周日 05:00 写入，TTL 7 天）→ ChatReviews 表冷路径（倒序最新）→
    /// 404（无对话记录或 Job 未跑）。review 为 LLM 结构化 JSON
    /// （summary/topics/highlights/suggestion/reviewWords 五段），设备
    /// cJSON 解析屏显；turnCount 供屏头统计。</remarks>
    [HttpGet("chat-review")]
    [ServiceFilter(typeof(DeviceAuthFilter))]
    public async Task<IActionResult> ChatReview(CancellationToken ct)
    {
        var device = (Device)HttpContext.Items["Device"]!;

        ChatReviewCache? cached = null;
        try
        {
            cached = await _cache.GetAsync<ChatReviewCache>(
                $"chatreview:{device.Id:N}", ct);
        }
        catch (Exception) { /* Redis 挂走冷路径，chat 降级同哲学 */ }
        if (cached is not null)
            return Ok(ApiResponse<object>.Ok(
                ToPayload(cached.WeekStart, cached.TurnCount, cached.PayloadJson)));

        var latest = await _db.ChatReviews.AsNoTracking()
            .Where(r => r.DeviceId == device.Id)
            .OrderByDescending(r => r.WeekStart)
            .FirstOrDefaultAsync(ct);
        if (latest is null)
            return NotFound(ApiResponse.Fail(404, "no review yet"));

        return Ok(ApiResponse<object>.Ok(
            ToPayload(latest.WeekStart, latest.TurnCount, latest.PayloadJson)));
    }

    /// <summary>周报下发载荷：review 解为 JSON 对象嵌入（设备免二次转义），
    /// 非法 JSON 兜底原文字符串</summary>
    private static object ToPayload(DateTime weekStart, int turnCount, string payloadJson) => new
    {
        weekStart,
        turnCount,
        review = JsonNode.Parse(payloadJson) ?? (object)payloadJson,
    };

    /// <summary>ApiKey 生成（public：MyDeviceController 绑定换发同源 + 测试直测，HashPassword 先例）</summary>
    public static string GenerateApiKey()
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
