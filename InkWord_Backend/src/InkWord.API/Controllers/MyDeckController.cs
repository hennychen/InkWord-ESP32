using System.Security.Claims;
using InkWord.Core.Common;
using InkWord.Infrastructure.DbContext;
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Mvc;
using Microsoft.EntityFrameworkCore;

namespace InkWord.API.Controllers;

/// <summary>
/// 学习者卡组 CRUD（v1.5 T5.3 卡组编辑器后端，ACCOUNT_MODEL_DECISION §四）。
///
/// 归属即权限：Deck.OwnerId == 当前账户才可读写；官方卡组（OwnerId=null）
/// 对学习者只读不可改。条目落 Words 表（T4.1 Item 混合模型）：Tag=deck.Code
/// 与官方库隔离（uq(Text,Tag)），Front/Back 双写、Version 全局递增——设备
/// 增量同步通道天然可用；App 端主推送通道是 LAN 直传（uploadDeck 全量
/// 覆盖 SD decks/&lt;id&gt;/words.json，设备零感知归属）。
/// 删除语义：Archived=true + Version++（GetIncrementalAsync 排除归档，
/// 设备侧不再拉到；已落 SD 的 LAN 拷贝独立，由 App 重推覆盖）。
/// </summary>
[ApiController]
[Route("api/me")]
[Authorize(Roles = "learner")]
public class MyDeckController : ControllerBase
{
    /// <summary>版式模板白名单（T4.3 card_layout 分派契约）</summary>
    private static readonly string[] PayloadTypes = ["word-card", "qa-card", "poem-card"];

    private readonly AppDbContext _db;

    public MyDeckController(AppDbContext db) => _db = db;

    public record ItemReq(string Front, string Back, string? Phonetic, string? Example);
    public record CreateDeckReq(
        string SubjectCode, string Name, string PayloadType,
        string? Description, List<ItemReq>? Items);
    public record UpdateDeckReq(string Name, string? Description);
    public record UpdateItemReq(string Front, string Back, string? Phonetic, string? Example);
    public record AddItemsReq(List<ItemReq> Items);

    public record DeckDto(
        Guid Id, string Code, string Name, string PayloadType, string Description,
        string SubjectCode, int ItemCount, DateTime UpdatedAt);
    public record ItemDto(Guid Id, int Order, string Front, string Back, string Phonetic, string Example);
    public record SubjectDto(string Code, string Name, int SortOrder);

    // ---- 查询 ----

    /// <summary>我的卡组列表（含条目数）</summary>
    [HttpGet("decks")]
    public async Task<IActionResult> List(CancellationToken ct)
    {
        var ownerId = OwnerId;
        var decks = await _db.Decks.AsNoTracking()
            .Where(d => d.OwnerId == ownerId)
            .OrderByDescending(d => d.UpdatedAt ?? d.CreatedAt)
            .ToListAsync(ct);
        var subjects = await _db.Subjects.AsNoTracking()
            .ToDictionaryAsync(s => s.Id, s => s.Code, ct);

        var counts = await _db.Words.AsNoTracking()
            .Where(w => w.DeckId != null && !w.Archived)
            .GroupBy(w => w.DeckId!.Value)
            .ToDictionaryAsync(g => g.Key, g => g.Count(), ct);

        var dto = decks.Select(d => new DeckDto(
            d.Id, d.Code, d.Name, d.PayloadType, d.Description,
            subjects.GetValueOrDefault(d.SubjectId, "en"),
            counts.GetValueOrDefault(d.Id, 0),
            d.UpdatedAt ?? d.CreatedAt)).ToList();
        return Ok(ApiResponse<List<DeckDto>>.Ok(dto));
    }

    /// <summary>科目清单（编辑器新建下拉；公共数据）</summary>
    [HttpGet("subjects")]
    public async Task<IActionResult> Subjects(CancellationToken ct)
    {
        var list = await _db.Subjects.AsNoTracking()
            .OrderBy(s => s.SortOrder)
            .Select(s => new SubjectDto(s.Code, s.Name, s.SortOrder))
            .ToListAsync(ct);
        return Ok(ApiResponse<List<SubjectDto>>.Ok(list));
    }

    /// <summary>卡组条目列表（编辑器打开卡组时拉取；Version 升序即录入序）</summary>
    [HttpGet("decks/{id}/items")]
    public async Task<IActionResult> Items(Guid id, CancellationToken ct)
    {
        var deck = await FindOwned(id, ct);
        if (deck == null) return NotFound(ApiResponse.Fail(404, "卡组不存在"));

        var items = await _db.Words.AsNoTracking()
            .Where(w => w.DeckId == id && !w.Archived)
            .OrderBy(w => w.Version)
            .Select(w => new ItemDto(w.Id, w.Version, w.Front, w.Back, w.Phonetic, w.Example))
            .ToListAsync(ct);
        return Ok(ApiResponse<List<ItemDto>>.Ok(items));
    }

    // ---- 卡组级 ----

    /// <summary>创建卡组（可携首批条目一步完成）。Code 自动生成
    /// （Guid 前 7 位 hex，≤7 字符同源设备 deck id 约束），冲突重试。</summary>
    [HttpPost("decks")]
    public async Task<IActionResult> Create([FromBody] CreateDeckReq req, CancellationToken ct)
    {
        var name = req.Name?.Trim() ?? "";
        if (name.Length is < 1 or > 128)
            return BadRequest(ApiResponse.Fail(400, "卡组名 1~128 字符"));
        if (!PayloadTypes.Contains(req.PayloadType))
            return BadRequest(ApiResponse.Fail(400,
                $"版式须为 {string.Join('/', PayloadTypes)}"));

        var subject = await _db.Subjects.AsNoTracking()
            .FirstOrDefaultAsync(s => s.Code == req.SubjectCode, ct);
        if (subject == null)
            return BadRequest(ApiResponse.Fail(400, $"科目 '{req.SubjectCode}' 不存在"));

        var deck = new InkWord.Core.Entities.Deck
        {
            SubjectId = subject.Id,
            Code = await NextCodeAsync(subject.Id, ct),
            Name = name,
            PayloadType = req.PayloadType,
            Description = req.Description ?? "",
            OwnerId = OwnerId,
        };
        _db.Decks.Add(deck);

        if (req.Items is { Count: > 0 })
        {
            int v = await _db.Words.AsNoTracking()
                .MaxAsync(w => (int?)w.Version, ct) ?? 0;
            foreach (var item in req.Items)
                _db.Words.Add(ToWord(deck, item, ++v));
        }
        await _db.SaveChangesAsync(ct);

        return Ok(ApiResponse<DeckDto>.Ok(new DeckDto(
            deck.Id, deck.Code, deck.Name, deck.PayloadType, deck.Description,
            subject.Code, req.Items?.Count ?? 0, deck.CreatedAt)));
    }

    /// <summary>改名/描述</summary>
    [HttpPut("decks/{id}")]
    public async Task<IActionResult> Update(Guid id, [FromBody] UpdateDeckReq req, CancellationToken ct)
    {
        var deck = await FindOwned(id, ct);
        if (deck == null) return NotFound(ApiResponse.Fail(404, "卡组不存在"));

        var name = req.Name?.Trim() ?? "";
        if (name.Length is < 1 or > 128)
            return BadRequest(ApiResponse.Fail(400, "卡组名 1~128 字符"));
        deck.Name = name;
        deck.Description = req.Description ?? deck.Description;
        await _db.SaveChangesAsync(ct);
        return Ok(ApiResponse<object>.Ok(new { deck.Id }));
    }

    /// <summary>删除卡组（Deck 软删 + 条目全部归档，Version++ 使设备
    /// 增量同步感知；LAN 落 SD 的拷贝独立，App 可另行重推清空）</summary>
    [HttpDelete("decks/{id}")]
    public async Task<IActionResult> Delete(Guid id, CancellationToken ct)
    {
        var deck = await FindOwned(id, ct);
        if (deck == null) return NotFound(ApiResponse.Fail(404, "卡组不存在"));

        int v = await _db.Words.AsNoTracking()
            .MaxAsync(w => (int?)w.Version, ct) ?? 0;
        var items = await _db.Words.Where(w => w.DeckId == id && !w.Archived).ToListAsync(ct);
        foreach (var w in items)
        {
            w.Archived = true;
            w.Version = ++v;
        }
        // 软删（RepositoryBase.DeleteAsync 同语义：IsDeleted + UpdatedAt，
        // 全局过滤器隐藏；此处裸 DbContext 直接置位）
        deck.IsDeleted = true;
        deck.UpdatedAt = DateTime.UtcNow;
        await _db.SaveChangesAsync(ct);
        return Ok(ApiResponse<object>.Ok(new { deck.Id }));
    }

    // ---- 条目级 ----

    /// <summary>批量追加条目（CSV/Excel 导入同端点）</summary>
    [HttpPost("decks/{id}/items")]
    public async Task<IActionResult> AddItems(
        Guid id, [FromBody] AddItemsReq req, CancellationToken ct)
    {
        var deck = await FindOwned(id, ct);
        if (deck == null) return NotFound(ApiResponse.Fail(404, "卡组不存在"));
        if (req.Items.Count == 0)
            return BadRequest(ApiResponse.Fail(400, "条目为空"));
        if (req.Items.Count > 2000)
            return BadRequest(ApiResponse.Fail(400, "单批 ≤2000 条"));

        int v = await _db.Words.AsNoTracking()
            .MaxAsync(w => (int?)w.Version, ct) ?? 0;
        foreach (var item in req.Items)
            _db.Words.Add(ToWord(deck, item, ++v));
        await _db.SaveChangesAsync(ct);
        return Ok(ApiResponse<object>.Ok(new { added = req.Items.Count }));
    }

    /// <summary>改条目（Version++ 供设备增量同步）</summary>
    [HttpPut("decks/{id}/items/{wordId}")]
    public async Task<IActionResult> UpdateItem(
        Guid id, Guid wordId, [FromBody] UpdateItemReq req, CancellationToken ct)
    {
        var deck = await FindOwned(id, ct);
        if (deck == null) return NotFound(ApiResponse.Fail(404, "卡组不存在"));

        var w = await _db.Words.FirstOrDefaultAsync(
            x => x.Id == wordId && x.DeckId == id, ct);
        if (w == null) return NotFound(ApiResponse.Fail(404, "条目不存在"));

        FillWord(w, req.Front, req.Back, req.Phonetic, req.Example);
        w.Version++;
        await _db.SaveChangesAsync(ct);
        return Ok(ApiResponse<object>.Ok(new { w.Id }));
    }

    /// <summary>删条目（归档 + Version++）</summary>
    [HttpDelete("decks/{id}/items/{wordId}")]
    public async Task<IActionResult> DeleteItem(Guid id, Guid wordId, CancellationToken ct)
    {
        var deck = await FindOwned(id, ct);
        if (deck == null) return NotFound(ApiResponse.Fail(404, "卡组不存在"));

        var w = await _db.Words.FirstOrDefaultAsync(
            x => x.Id == wordId && x.DeckId == id, ct);
        if (w == null) return NotFound(ApiResponse.Fail(404, "条目不存在"));

        w.Archived = true;
        w.Version++;
        await _db.SaveChangesAsync(ct);
        return Ok(ApiResponse<object>.Ok(new { w.Id }));
    }

    // ---- 内部 ----

    private Guid OwnerId =>
        Guid.TryParse(User.FindFirstValue(ClaimTypes.NameIdentifier), out var g)
            ? g
            : throw new UnauthorizedAccessException("token 缺少账户标识");

    /// <summary>归属校验（OwnerId 匹配才可见可改）</summary>
    private Task<InkWord.Core.Entities.Deck?> FindOwned(Guid id, CancellationToken ct) =>
        _db.Decks.FirstOrDefaultAsync(d => d.Id == id && d.OwnerId == OwnerId, ct);

    /// <summary>Code 生成：Guid N 前 7 位（设备 deck id 约束同源），冲突重试</summary>
    internal async Task<string> NextCodeAsync(Guid subjectId, CancellationToken ct)
    {
        for (var i = 0; i < 5; i++)
        {
            var code = "u" + Guid.NewGuid().ToString("N")[..6];
            if (!await _db.Decks.AsNoTracking()
                    .AnyAsync(d => d.SubjectId == subjectId && d.Code == code, ct))
                return code;
        }
        throw new InvalidOperationException("code 生成冲突（重试后仍占用）");
    }

    /// <summary>条目统一映射：Text/Front=正面，Meaning/Back=背面
    /// （设备 WordEntry 语义泛化：qa 题面/诗上句走 text，答案/下句走
    /// meaning）。public static 供测试直测。</summary>
    private static InkWord.Core.Entities.Word ToWord(
        InkWord.Core.Entities.Deck deck, ItemReq item, int version)
    {
        var w = new InkWord.Core.Entities.Word { Version = version, Tag = deck.Code };
        FillWord(w, item.Front, item.Back, item.Phonetic, item.Example);
        w.SubjectId = deck.SubjectId;
        w.DeckId = deck.Id;
        return w;
    }

    public static void FillWord(
        InkWord.Core.Entities.Word w,
        string front, string back, string? phonetic, string? example)
    {
        w.Text = (front ?? "").Trim()[..Math.Min(128, (front ?? "").Trim().Length)];
        w.Meaning = back ?? "";
        w.Front = w.Text;
        w.Back = back ?? "";
        w.Phonetic = phonetic ?? "";
        w.Example = example ?? "";
    }
}
