using System.Text;
using Hangfire;
using InkWord.API.DTOs;
using InkWord.Core.Common;
using InkWord.Core.Entities;
using InkWord.Core.Repositories;
using InkWord.Infrastructure.Repositories;
using InkWord.Jobs;
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Mvc;
using Microsoft.EntityFrameworkCore;

namespace InkWord.API.Controllers;

/// <summary>
/// 管理端卡组接口（v1.4 T4.5 字库子集下发；v1.5 T5.4 AI 卡组生成；
/// P3 卡组/条目管理 2026-09：列表增强 + 详情 + 条目 CRUD）。
/// 条目写路径与 me 端同源：FillWord/NextVersion 公共方法复用，
/// Tag=deck.Code 隔离，Front/Back 双写、Version 接全局 max 递增
/// （T4.1 契约红线，禁复制）。
/// </summary>
[ApiController]
[Route("api/admin/decks")]
[Authorize(Roles = "Admin,Operator")]
public class AdminDeckController : ControllerBase
{
    private readonly IUnitOfWork _uow;
    private readonly IWordRepository _wordRepo;

    public AdminDeckController(IUnitOfWork uow, IWordRepository wordRepo)
    {
        _uow = uow;
        _wordRepo = wordRepo;
    }

    /// <summary>卡组列表（P3 卡组管理页数据源；subject=科目 Code 过滤；
/// 含条目计数/共享态/Owner 展示名，前端 chip 侧栏过滤同源）</summary>
    [HttpGet]
    public async Task<IActionResult> List([FromQuery] string? subject, CancellationToken ct)
    {
        var query = _uow.Db.Decks.AsNoTracking().Where(d => !d.IsDeleted);
        if (!string.IsNullOrWhiteSpace(subject))
        {
            var sid = await _uow.Db.Subjects.AsNoTracking()
                .Where(s => s.Code == subject)
                .Select(s => (Guid?)s.Id)
                .FirstOrDefaultAsync(ct);
            if (sid == null)
                return Ok(ApiResponse<object>.Ok(new List<object>()));
            query = query.Where(d => d.SubjectId == sid.Value);
        }

        var decks = await query.OrderBy(d => d.Code).ToListAsync(ct);
        var subjects = await _uow.Db.Subjects.AsNoTracking()
            .ToDictionaryAsync(s => s.Id, s => (s.Code, s.Name), ct);
        var ownerIds = decks.Where(d => d.OwnerId != null)
            .Select(d => d.OwnerId!.Value).Distinct().ToList();
        var owners = ownerIds.Count == 0
            ? new Dictionary<Guid, string>()
            : await _uow.Db.Accounts.AsNoTracking()
                .Where(a => ownerIds.Contains(a.Id))
                .ToDictionaryAsync(a => a.Id, a => a.DisplayName, ct);
        var counts = await _uow.Db.Words.AsNoTracking()
            .Where(w => w.DeckId != null && !w.Archived)
            .GroupBy(w => w.DeckId!.Value)
            .ToDictionaryAsync(g => g.Key, g => g.Count(), ct);
        var maxVers = await _uow.Db.Words.AsNoTracking()
            .Where(w => w.DeckId != null && !w.Archived)
            .GroupBy(w => w.DeckId!.Value)
            .ToDictionaryAsync(g => g.Key, g => g.Max(w => w.Version), ct);

        var dto = decks.Select(d => new
        {
            d.Id, d.Code, d.Name, d.PayloadType, d.SubjectId,
            ItemCount = counts.GetValueOrDefault(d.Id, 0),
            maxVersion = maxVers.GetValueOrDefault(d.Id, 0),
            subjectCode = subjects.GetValueOrDefault(d.SubjectId).Code ?? "",
            subjectName = subjects.GetValueOrDefault(d.SubjectId).Name ?? "",
            d.IsShared,
            ownerName = d.OwnerId == null
                ? "官方"
                : owners.GetValueOrDefault(d.OwnerId.Value, "—"),
            d.UpdatedAt,
        }).ToList();
        return Ok(ApiResponse<object>.Ok(dto));
    }

    /// <summary>T5.4 AI 批量生成卡组：素材（课文/知识点清单）→ 占位待审条目
    /// （Version=0 不下发）；AI 审核台通过后 Version++ 增量下发（设备零改动红线）。</summary>
    [HttpPost("{id:guid}/ai-generate")]
    public IActionResult AiGenerate(Guid id, [FromBody] DeckGenReq req)
    {
        var source = req.Source?.Trim();
        if (string.IsNullOrEmpty(source))
            return BadRequest(ApiResponse.Fail(400, "素材不能为空"));
        var jobId = BackgroundJob.Enqueue<DeckGenJob>(
            j => j.RunAsync(id, source, req.Limit, CancellationToken.None));
        return Ok(ApiResponse<object>.Ok(new { jobId }, "AI 卡组生成任务已入队"));
    }

    /// <summary>
    /// T4.5 卡组字符集导出：该卡组全部词条设备端可渲染九列（text/phonetic/
    /// meaning/example/root/inflections/source/grade/tag）的去重字符流，
    /// text/plain; charset=utf-8。喂给 tools/gen_cjk_font.swift --subset 与
    /// 主集（GB2312 一级 + IPA + 标点 + ASCII）做差集，产出 deck_&lt;code&gt;.bin
    /// 拷入 SD /fonts/ 供固件 cjk_font_sd 级联查找（生僻字刚需，主集 miss →
    /// 子集命中）。payloadJson 设备端零解析（T4.4 契约），不收集。
    /// </summary>
    [HttpGet("{code}/charset")]
    public async Task<IActionResult> ExportCharset(string code, CancellationToken ct)
    {
        var deck = await _uow.Db.Decks.AsNoTracking()
            .FirstOrDefaultAsync(d => d.Code == code, ct);
        if (deck == null)
            return NotFound(ApiResponse.Fail(404, $"deck '{code}' not found"));

        // Word 侧纯 Id 关联（T4.1 三表 Item 混合模型不建导航），显式按 DeckId 过滤
        var words = await _uow.Db.Words.AsNoTracking()
            .Where(w => w.DeckId == deck.Id && !w.IsDeleted)
            .Select(w => new
            {
                w.Text, w.Phonetic, w.Meaning, w.Example,
                w.Root, w.Inflections, w.Source, w.Grade, w.Tag,
            })
            .ToListAsync(ct);

        var seen = new HashSet<char>();
        var sb = new StringBuilder();
        void Feed(string? s)
        {
            if (string.IsNullOrEmpty(s)) return;
            foreach (var ch in s)
                if (seen.Add(ch)) sb.Append(ch);
        }
        foreach (var w in words)
        {
            Feed(w.Text); Feed(w.Phonetic); Feed(w.Meaning); Feed(w.Example);
            Feed(w.Root); Feed(w.Inflections); Feed(w.Source); Feed(w.Grade);
            Feed(w.Tag);
        }

        return new ContentResult
        {
            Content = sb.ToString(),
            ContentType = "text/plain; charset=utf-8",
            StatusCode = StatusCodes.Status200OK,
        };
    }

    // ====== P3 卡组/条目管理（2026-09） ======

    public record AdminItemReq(string Front, string Back, string? Phonetic, string? Example);

    /// <summary>卡组详情：元数据 + Owner + 学习覆盖统计
    /// （learnerCount 学过本组条目的设备数 / studiedItems 被学条目数，
/// LearningRecord join Words 关联统计）。</summary>
    [HttpGet("{id:guid}")]
    public async Task<IActionResult> Detail(Guid id, CancellationToken ct)
    {
        var deck = await _uow.Db.Decks.AsNoTracking()
            .FirstOrDefaultAsync(d => d.Id == id && !d.IsDeleted, ct);
        if (deck == null) return NotFound(ApiResponse.Fail(404, "deck not found"));

        var subject = await _uow.Db.Subjects.AsNoTracking()
            .Where(s => s.Id == deck.SubjectId)
            .Select(s => new { s.Code, s.Name })
            .FirstOrDefaultAsync(ct);

        string? ownerName = null;
        if (deck.OwnerId != null)
            ownerName = await _uow.Db.Accounts.AsNoTracking()
                .Where(a => a.Id == deck.OwnerId)
                .Select(a => a.DisplayName)
                .FirstOrDefaultAsync(ct);

        var itemCount = await _uow.Db.Words.AsNoTracking()
            .CountAsync(w => w.DeckId == id && !w.Archived, ct);
        var maxVersion = await _uow.Db.Words.AsNoTracking()
            .Where(w => w.DeckId == id && !w.Archived)
            .MaxAsync(w => (int?)w.Version, ct) ?? 0;

        var covered = _uow.Db.LearningRecords.AsNoTracking()
            .Where(lr => _uow.Db.Words.Any(w => w.Id == lr.WordId && w.DeckId == id));
        var learnerCount = await covered.Select(lr => lr.DeviceId).Distinct().CountAsync(ct);
        var studiedItems = await covered.Select(lr => lr.WordId).Distinct().CountAsync(ct);

        return Ok(ApiResponse<object>.Ok(new
        {
            deck.Id, deck.Code, deck.Name, deck.PayloadType, deck.Description,
            deck.IsShared, deck.SharedAt, deck.CreatedAt,
            subjectCode = subject?.Code ?? "",
            subjectName = subject?.Name ?? "",
            ownerId = deck.OwnerId,
            ownerName,
            itemCount, maxVersion, learnerCount, studiedItems,
        }));
    }

    /// <summary>条目分页（P3 条目抽屉）：CreatedAt 升序 = 录入序
    /// （排序键与 Version 解耦——写操作 Version 接全局 max 递增后
    /// 不再单调于录入序，按 Version 排会把被编辑行甩到末页）；
    /// 含归档行（archived 标记，可查删除痕迹）。</summary>
    [HttpGet("{id:guid}/items")]
    public async Task<IActionResult> Items(
        Guid id, [FromQuery] int page = 1, [FromQuery] int size = 20,
        CancellationToken ct = default)
    {
        if (page < 1) page = 1;
        if (size is < 1 or > 200) size = 20;

        var query = _uow.Db.Words.AsNoTracking().Where(w => w.DeckId == id);
        var total = await query.CountAsync(ct);
        var items = await query
            .OrderBy(w => w.CreatedAt).ThenBy(w => w.Version)
            .Skip((page - 1) * size).Take(size)
            .Select(w => new
            {
                w.Id, w.Version, w.Text,
                Front = w.Front ?? "", Back = w.Back ?? "",
                w.Phonetic, w.Meaning, w.Example, w.Archived,
            })
            .ToListAsync(ct);
        return Ok(ApiResponse<object>.Ok(new { items, total, page, size }));
    }

    /// <summary>新增条目（单条）：FillWord 复用 me 端同源映射（Front/Back
    /// 双写、Text 截 128），Tag=deck.Code 隔离，Version 接全局 max 递增；
    /// uq(Text,Tag) 冲突 409（含归档行，防复活撞索引）。</summary>
    [HttpPost("{id:guid}/items")]
    public async Task<IActionResult> AddItem(
        Guid id, [FromBody] AdminItemReq req, CancellationToken ct)
    {
        var deck = await FindDeck(id, ct);
        if (deck == null) return NotFound(ApiResponse.Fail(404, "deck not found"));
        if (string.IsNullOrWhiteSpace(req.Front))
            return BadRequest(ApiResponse.Fail(400, "正面（Front）不能为空"));

        var text = req.Front.Trim();
        if (await _wordRepo.ExistsByTextAsync(text, deck.Code, ct))
            return Conflict(ApiResponse.Fail(409,
                $"条目 '{text}' 已存在于卡组 '{deck.Code}'"));

        var w = new Word
        {
            Version = MyDeckController.NextVersion(0, await _wordRepo.GetMaxVersionAsync(ct)),
            Tag = deck.Code,
            SubjectId = deck.SubjectId,
            DeckId = deck.Id,
        };
        MyDeckController.FillWord(w, req.Front, req.Back, req.Phonetic, req.Example);
        await _wordRepo.AddAsync(w, ct);
        await _wordRepo.SaveChangesAsync(ct);
        return Ok(ApiResponse<object>.Ok(new { w.Id, w.Version }));
    }

    /// <summary>改条目：FillWord 同源映射 + Version 接全局 max 递增
    /// （设备增量同步下发契约）；Text 变更时重查 uq(Text,Tag) 防 409。</summary>
    [HttpPut("{id:guid}/items/{wordId:guid}")]
    public async Task<IActionResult> UpdateItem(
        Guid id, Guid wordId, [FromBody] AdminItemReq req, CancellationToken ct)
    {
        var deck = await FindDeck(id, ct);
        if (deck == null) return NotFound(ApiResponse.Fail(404, "deck not found"));

        var w = await _uow.Db.Words.FirstOrDefaultAsync(
            x => x.Id == wordId && x.DeckId == id, ct);
        if (w == null) return NotFound(ApiResponse.Fail(404, "条目不存在"));
        if (string.IsNullOrWhiteSpace(req.Front))
            return BadRequest(ApiResponse.Fail(400, "正面（Front）不能为空"));

        var text = req.Front.Trim();
        if (text != w.Text && await _wordRepo.ExistsByTextAsync(text, deck.Code, ct))
            return Conflict(ApiResponse.Fail(409,
                $"条目 '{text}' 已存在于卡组 '{deck.Code}'"));

        MyDeckController.FillWord(w, req.Front, req.Back, req.Phonetic, req.Example);
        w.Version = MyDeckController.NextVersion(
            w.Version, await _wordRepo.GetMaxVersionAsync(ct));
        await _wordRepo.SaveChangesAsync(ct);
        return Ok(ApiResponse<object>.Ok(new { w.Id, w.Version }));
    }

    /// <summary>删条目（归档 + Version 接 max 递增；同 me 端 DeleteItem 语义，
    /// 已落 SD 的 LAN 拷贝独立，由 App 重推覆盖）</summary>
    [HttpDelete("{id:guid}/items/{wordId:guid}")]
    public async Task<IActionResult> DeleteItem(Guid id, Guid wordId, CancellationToken ct)
    {
        var deck = await FindDeck(id, ct);
        if (deck == null) return NotFound(ApiResponse.Fail(404, "deck not found"));

        var w = await _uow.Db.Words.FirstOrDefaultAsync(
            x => x.Id == wordId && x.DeckId == id, ct);
        if (w == null) return NotFound(ApiResponse.Fail(404, "条目不存在"));

        w.Archived = true;
        w.Version = MyDeckController.NextVersion(
            w.Version, await _wordRepo.GetMaxVersionAsync(ct));
        await _wordRepo.SaveChangesAsync(ct);
        return Ok(ApiResponse<object>.Ok(new { w.Id }));
    }

    // ---- 内部 ----

    /// <summary>管理端可见卡组（未软删；含官方与学习者卡组——与 me 端
    /// 归属即权限不同，管理端全量可管）</summary>
    private Task<Deck?> FindDeck(Guid id, CancellationToken ct) =>
        _uow.Db.Decks.FirstOrDefaultAsync(d => d.Id == id && !d.IsDeleted, ct);
}
