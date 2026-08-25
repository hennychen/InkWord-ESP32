using System.Globalization;
using System.Text;
using System.Text.Json;
using Hangfire;
using InkWord.API.DTOs;
using InkWord.Core.Common;
using InkWord.Core.Entities;
using InkWord.Core.Repositories;
using InkWord.Infrastructure.DbContext;
using InkWord.Jobs;
using InkWord.Services;
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Mvc;
using Microsoft.EntityFrameworkCore;

namespace InkWord.API.Controllers;

/// <summary>
/// 管理端词库接口（供 Angular 调用）。
/// </summary>
[ApiController]
[Route("api/admin/words")]
[Authorize(Roles = "Admin,Operator")]
public class AdminWordController : ControllerBase
{
    private readonly IWordRepository _wordRepo;
    private readonly AiContentService _ai;
    private readonly AppDbContext _db; // T4.1：export v2 归属映射

    public AdminWordController(IWordRepository wordRepo, AiContentService ai, AppDbContext db)
    {
        _wordRepo = wordRepo;
        _ai = ai;
        _db = db;
    }

    /// <summary>B-13 单条新增（含去重校验）</summary>
    [HttpPost]
    public async Task<IActionResult> Create([FromBody] WordCreateDto dto, CancellationToken ct)
    {
        if (await _wordRepo.ExistsByTextAsync(dto.Text, dto.Tag, ct))
            return Conflict(ApiResponse.Fail(409, $"word '{dto.Text}' already exists in tag '{dto.Tag}'"));

        var maxVer = await _wordRepo.GetMaxVersionAsync(ct);
        var word = new Word
        {
            Text = dto.Text,
            Phonetic = dto.Phonetic ?? "",
            Meaning = dto.Meaning ?? "",
            Example = dto.Example ?? "",
            Audio = dto.Audio ?? "",
            Tag = dto.Tag ?? "",
            Root = dto.Root ?? "",
            Inflections = dto.Inflections ?? "",
            Source = dto.Source ?? "",
            Grade = dto.Grade ?? "",
            Difficulty = dto.Difficulty,
            Version = maxVer + 1,
            ChangeType = 0,
            // v2 卡面冗余镜像（T4.1）：word-card 的 Front/Back 随写随同步
            Front = dto.Text,
            Back = dto.Meaning ?? "",
        };
        await _wordRepo.AddAsync(word, ct);
        await _wordRepo.SaveChangesAsync(ct);
        return Ok(ApiResponse<Word>.Ok(word));
    }

    /// <summary>B-13 单条修改</summary>
    [HttpPut("{id:guid}")]
    public async Task<IActionResult> Update(Guid id, [FromBody] WordUpdateDto dto, CancellationToken ct)
    {
        var word = await _wordRepo.GetByIdAsync(id, ct);
        if (word == null) return NotFound(ApiResponse.Fail(404, "not found"));

        word.Text = dto.Text;
        word.Phonetic = dto.Phonetic ?? "";
        word.Meaning = dto.Meaning ?? "";
        word.Example = dto.Example ?? "";
        word.Audio = dto.Audio ?? "";
        word.Tag = dto.Tag ?? "";
        word.Root = dto.Root ?? "";
        word.Inflections = dto.Inflections ?? "";
        word.Source = dto.Source ?? "";
        word.Grade = dto.Grade ?? "";
        word.Difficulty = dto.Difficulty;
        word.ChangeType = 1;
        word.Version = Math.Max(word.Version, await _wordRepo.GetMaxVersionAsync(ct)) + 1;
        word.Front = dto.Text;             // v2 卡面镜像随写同步（T4.1）
        word.Back = dto.Meaning ?? "";

        await _wordRepo.UpdateAsync(word, ct);
        await _wordRepo.SaveChangesAsync(ct);
        return Ok(ApiResponse<Word>.Ok(word));
    }

    /// <summary>B-13 软删除</summary>
    [HttpDelete("{id:guid}")]
    public async Task<IActionResult> Delete(Guid id, CancellationToken ct)
    {
        var word = await _wordRepo.GetByIdAsync(id, ct);
        if (word == null) return NotFound(ApiResponse.Fail(404, "not found"));

        await _wordRepo.DeleteAsync(word, ct);
        await _wordRepo.SaveChangesAsync(ct);
        return Ok(ApiResponse.Ok());
    }

    /// <summary>B-15 分页 + 多条件筛选</summary>
    [HttpGet]
    public async Task<IActionResult> List([FromQuery] WordQueryDto q, CancellationToken ct)
    {
        var (items, total) = await _wordRepo.GetPagedAsync(q.Page, q.Size, w =>
            (!q.Difficulty.HasValue || w.Difficulty == q.Difficulty.Value) &&
            (string.IsNullOrEmpty(q.Tag) || w.Tag == q.Tag) &&
            (string.IsNullOrEmpty(q.Keyword) ||
             EF.Functions.ILike(w.Text, $"%{q.Keyword}%") ||
             EF.Functions.ILike(w.Meaning, $"%{q.Keyword}%")), ct);

        return Ok(ApiResponse<PagedResult<Word>>.Ok(new PagedResult<Word>(items, total, q.Page, q.Size)));
    }

    /// <summary>B-14 批量导入 CSV（首行表头：text,phonetic,meaning,example,audio,tag,difficulty,
    ///     root,inflections,source,grade —— 后四列可缺省，列序固定）。tag 查询参数
    /// 可选：缺省时回退 CSV 第 6 列（管理端导入弹窗不传 tag，2026-08-23
    /// 实测 .NET 8 非可空 string 隐式必填致 UI 导入全 400，改 string? 修复）</summary>
    [HttpPost("import")]
    public async Task<IActionResult> ImportCsv(IFormFile file, [FromQuery] string? tag, CancellationToken ct)
    {
        if (file == null || file.Length == 0)
            return BadRequest(ApiResponse.Fail(400, "empty file"));

        var ok = 0; var fail = 0; var errors = new List<string>();
        var maxVer = await _wordRepo.GetMaxVersionAsync(ct);

        using var stream = file.OpenReadStream();
        using var reader = new StreamReader(stream, Encoding.UTF8);
        // 跳过表头
        await reader.ReadLineAsync(ct);

        string? line;
        while ((line = await reader.ReadLineAsync(ct)) != null)
        {
            if (string.IsNullOrWhiteSpace(line)) continue;
            var f = line.Split(',');
            if (f.Length < 3) { fail++; errors.Add($"字段不足: {line}"); continue; }

            var text = f[0].Trim();
            // 行级 Tag（查询参数优先，否则 CSV 第 6 列兜底）：查重与落库
            // 同源。不能拿原始 tag（可能为 null）去查重 —— ExistsByTextAsync
            // 对空 tag 跳过过滤成全库按 text 判重，会误拒跨 Tag 合法词条
            //（唯一索引为 Text+Tag，如《论语》十二章分属初中/高考）。
            var rowTag = !string.IsNullOrEmpty(tag) ? tag : (f.Length > 5 ? f[5].Trim() : "");
            if (await _wordRepo.ExistsByTextAsync(text, rowTag, ct))
            { fail++; errors.Add($"重复: {text}"); continue; }

            var word = new Word
            {
                Text = text,
                Phonetic = f.Length > 1 ? f[1].Trim() : "",
                Meaning = f.Length > 2 ? f[2].Trim() : "",
                Example = f.Length > 3 ? f[3].Trim() : "",
                Audio = f.Length > 4 ? f[4].Trim() : "",
                Tag = rowTag,
                Difficulty = f.Length > 6 && int.TryParse(f[6], out var d) ? d : 1,
                Root = f.Length > 7 ? f[7].Trim() : "",
                Inflections = f.Length > 8 ? f[8].Trim() : "",
                Source = f.Length > 9 ? f[9].Trim() : "",
                Grade = f.Length > 10 ? f[10].Trim() : "",
                Version = ++maxVer,
                ChangeType = 0,
                Front = text,                          // v2 卡面镜像（T4.1）
                Back = f.Length > 2 ? f[2].Trim() : "",
            };
            await _wordRepo.AddAsync(word, ct);
            ok++;
        }
        await _wordRepo.SaveChangesAsync(ct);

        return Ok(ApiResponse<object>.Ok(new { success = ok, failed = fail, errors }, $"imported {ok} words"));
    }

    /// <summary>B-16 导出设备词库文件（words.json）：设备端评分/收藏上报的
    /// Guid 映射入口。id 为设备本地序号（依赖导出顺序稳定，顺序/规模变化
    /// 会触发设备侧学习状态整体作废重建），cloudId 为云端词条身份，
    /// 设备据此前报 WordId（P2 上报闭环，见固件 word_parser/sync_client）。
    /// v2（T4.1 全科地基）：新增 subject/deckId/payloadType/front/back/
    /// payloadJson 六字段与旧字段过渡期双写；旧固件 cJSON 忽略未知键
    /// （native 用例固化），cloudId/Version/ChangeType 语义不变。</summary>
    [HttpGet("export")]
    public async Task<IActionResult> Export(CancellationToken ct)
    {
        var words = await _wordRepo.GetIncrementalAsync(0, 100_000, ct);
        var version = await _wordRepo.GetMaxVersionAsync(ct);

        // deck/subject 身份映射（表行数个位数；与 DeviceController.SyncWords
        // 同源逻辑）：Word.SubjectId/DeckId 直查，null/失配兜底 en/junior。
        var deckById = await _db.Decks.AsNoTracking()
            .ToDictionaryAsync(d => d.Id, ct);
        var subCodes = await _db.Subjects.AsNoTracking()
            .ToDictionaryAsync(s => s.Id, s => s.Code, ct);

        var payload = new
        {
            version,
            v = 2,
            words = words.Select((w, i) =>
            {
                var deck = w.DeckId.HasValue && deckById.TryGetValue(w.DeckId.Value, out var d) ? d : null;
                var subject = w.SubjectId.HasValue && subCodes.TryGetValue(w.SubjectId.Value, out var sc)
                    ? sc
                    : deck != null ? subCodes.GetValueOrDefault(deck.SubjectId, "en") : "en";
                return new
                {
                    id = i + 1,
                    cloudId = w.Id.ToString(),
                    w.Text, w.Phonetic, w.Meaning, w.Example, w.Audio, w.Tag, w.Difficulty,
                    w.Root, w.Inflections, w.Source, w.Grade,
                    // ---- v2 全科地基 ----
                    subject,
                    deckId = deck?.Code ?? "junior",
                    payloadType = deck?.PayloadType ?? "word-card",
                    front = w.Front != "" ? w.Front : w.Text,
                    back = w.Back != "" ? w.Back : w.Meaning,
                    payloadJson = w.PayloadJson ?? "",
                };
            }),
        };

        var json = JsonSerializer.Serialize(payload,
            new JsonSerializerOptions(JsonSerializerDefaults.Web));
        return File(System.Text.Encoding.UTF8.GetBytes(json),
            "application/json", "words.json");
    }

    // ====== AI 内容增强（M1 路径 B，2026-08-22）======

    /// <summary>手动触发 AI 批量生成（立即入 Hangfire 队列，进度见 /hangfire）</summary>
    [HttpPost("ai-generate")]
    public IActionResult AiGenerate([FromBody] AiGenerateReq req)
    {
        if (req.Kind is < 0 or > 2)
            return BadRequest(ApiResponse.Fail(400, "kind must be 0/1/2"));
        var jobId = BackgroundJob.Enqueue<AiContentJob>(
            j => j.RunAsync(req.Kind, req.Tag, req.Subject, req.Limit, CancellationToken.None));
        return Ok(ApiResponse<object>.Ok(new { jobId }, "AI 生成任务已入队"));
    }

    /// <summary>P0B 手动触发词条 TTS 批量合成（立即入 Hangfire 队列）。</summary>
    /// <remarks>补齐 data/audio/{Id:N}.mp3 缺失词条；幂等可重复触发。</remarks>
    [HttpPost("tts-generate")]
    public IActionResult TtsGenerate([FromQuery] int limit)
    {
        limit = limit <= 0 || limit > 2000 ? 200 : limit;
        var jobId = BackgroundJob.Enqueue<TtsJob>(
            j => j.RunAsync(limit, CancellationToken.None));
        return Ok(ApiResponse<object>.Ok(new { jobId }, "TTS 合成任务已入队"));
    }

    /// <summary>待审建议分页（AiStatus=1；服务端解析 AiSuggestion 下发 diff 视图）</summary>
    [HttpGet("ai-pending")]
    public async Task<IActionResult> AiPending([FromQuery] int page, [FromQuery] int size, CancellationToken ct)
    {
        page = page <= 0 ? 1 : page;
        size = size <= 0 || size > 100 ? 20 : size;

        var (words, total) = await _wordRepo.GetPagedAsync(page, size,
            w => w.AiStatus == 1, ct);

        var items = words.Select(ToPendingItem).ToList();
        return Ok(ApiResponse<PagedResult<AiPendingItem>>.Ok(
            new PagedResult<AiPendingItem>(items, total, page, size)));
    }

    /// <summary>待审数量（词库页角标）</summary>
    [HttpGet("ai-pending/count")]
    public async Task<IActionResult> AiPendingCount(CancellationToken ct)
    {
        var (_, total) = await _wordRepo.GetPagedAsync(1, 1, w => w.AiStatus == 1, ct);
        return Ok(ApiResponse<object>.Ok(new { count = total }));
    }

    /// <summary>审核通过：建议写入词库字段并 Version++ 增量下发（可携带编辑终值）。</summary>
    [HttpPost("ai-apply/{id:guid}")]
    public async Task<IActionResult> AiApply(Guid id, [FromBody] AiApplyReq req, CancellationToken ct)
    {
        var word = await _wordRepo.GetByIdAsync(id, ct);
        if (word == null) return NotFound(ApiResponse.Fail(404, "not found"));
        if (word.AiStatus != 1 || string.IsNullOrEmpty(word.AiSuggestion))
            return BadRequest(ApiResponse.Fail(400, "无待审建议"));

        var sug = ParseSuggestion(word.AiSuggestion);
        if (sug == null) return BadRequest(ApiResponse.Fail(400, "建议载荷损坏，请驳回重新生成"));

        // 应用：kind 0 写 Example，kind 1 写 Root；kind 2（易混辨析）仅审阅参考不落设备字段。
        // 长度红线：字节上限对齐固件缓冲（AiContentService 同源常量）。
        if (sug.Kind == 0)
        {
            var example = req.Example ?? sug.Example ?? word.Example;
            if (Encoding.UTF8.GetByteCount(example) > AiContentService.ExampleMaxBytes)
                return BadRequest(ApiResponse.Fail(400,
                    $"例句超长（{Encoding.UTF8.GetByteCount(example)}B > {AiContentService.ExampleMaxBytes}B）"));
            word.Example = example;
        }
        else if (sug.Kind == 1)
        {
            var root = req.Root ?? sug.Root ?? word.Root;
            if (Encoding.UTF8.GetByteCount(root) > AiContentService.RootMaxBytes)
                return BadRequest(ApiResponse.Fail(400,
                    $"助记超长（{Encoding.UTF8.GetByteCount(root)}B > {AiContentService.RootMaxBytes}B）"));
            word.Root = root;
        }
        else if (sug.Kind == 3)
        {
            // T5.4 卡组条目：按目标卡组版式映射写入正字段（AiContentService
            // 内部按版式分派 + 字节截断；poem 对齐 T4.4 云通道契约，word/qa
            // 对齐 T4.3）。req 携人工编辑终值（优先于建议原值）
            var payloadType = await _db.Decks.AsNoTracking()
                .Where(d => d.Id == word.DeckId)
                .Select(d => d.PayloadType)
                .FirstOrDefaultAsync(ct);
            AiContentService.ApplyDeckSuggestion(word, payloadType, sug,
                req.Front, req.Back, req.Phonetic, req.Meaning, req.Example);
        }

        // 增量下发通道（照抄 Update 逻辑）：版本取全局最大 +1，变更类型 = 修改
        word.ChangeType = 1;
        word.Version = Math.Max(word.Version, await _wordRepo.GetMaxVersionAsync(ct)) + 1;
        word.AiStatus = 2;
        word.AiSuggestion = null;

        await _wordRepo.UpdateAsync(word, ct);
        await _wordRepo.SaveChangesAsync(ct);
        return Ok(ApiResponse<Word>.Ok(word));
    }

    /// <summary>驳回/失败复位：AiStatus 归 0（可重新生成）并清建议缓存。
    /// 1=驳回待审建议；3=复位生成失败词（否则永不被 AiStatus==0 查询重选）。</summary>
    [HttpPost("ai-reject/{id:guid}")]
    public async Task<IActionResult> AiReject(Guid id, CancellationToken ct)
    {
        var word = await _wordRepo.GetByIdAsync(id, ct);
        if (word == null) return NotFound(ApiResponse.Fail(404, "not found"));
        if (word.AiStatus is not (1 or 3))
            return BadRequest(ApiResponse.Fail(400, "无待审建议或失败记录"));

        var sug = ParseSuggestion(word.AiSuggestion);
        word.AiStatus = 0;
        word.AiSuggestion = null;
        await _wordRepo.UpdateAsync(word, ct);
        await _wordRepo.SaveChangesAsync(ct);

        if (sug != null)
            await _ai.InvalidateCacheAsync(word.Id, (AiContentKind)sug.Kind, ct);
        return Ok(ApiResponse.Ok());
    }

    private static AiPendingItem ToPendingItem(Word w)
    {
        var sug = ParseSuggestion(w.AiSuggestion);
        return new AiPendingItem(w.Id, w.Text, w.Meaning, w.Tag, w.Grade,
            w.Example, w.Root,
            sug?.Example, sug?.Root, sug?.ConfusionNote,
            sug?.Kind ?? 0, w.CreatedAt,
            sug?.Front, sug?.Back, sug?.Phonetic, sug?.Meaning);
    }

    private static WordAiSuggestion? ParseSuggestion(string? json)
    {
        if (string.IsNullOrEmpty(json)) return null;
        try
        {
            return JsonSerializer.Deserialize<WordAiSuggestion>(json,
                new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.CamelCase });
        }
        catch (JsonException)
        {
            return null;
        }
    }
}
