using System.Security.Claims;
using InkWord.Core.Common;
using InkWord.Infrastructure.Repositories;
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
/// 删除语义：Archived=true + Version 接 max 递增（GetIncrementalAsync 排除归档，
/// 设备侧不再拉到；已落 SD 的 LAN 拷贝独立，由 App 重推覆盖）。
///
/// UGC 分享（v2.0 #3 生态首增量）：share 开关 → decks/shared 发现页
/// → fork 深拷贝导入。导入是独立副本（新版式 Code + 条目全量拷贝），
/// 后续与源互不影响；关闭分享不回收已导入副本。设备零改动——fork
/// 后走既有 LAN 推送 / 增量同步通道（ACCOUNT_MODEL_DECISION §五生态）。
/// </summary>
[ApiController]
[Route("api/me")]
[Authorize(Roles = "learner")]
public class MyDeckController : ControllerBase
{
    /// <summary>版式模板白名单（T4.3 card_layout 分派契约）</summary>
    private static readonly string[] PayloadTypes = ["word-card", "qa-card", "poem-card"];

    private readonly IUnitOfWork _uow;

    public MyDeckController(IUnitOfWork uow) => _uow = uow;

    public record ItemReq(string Front, string Back, string? Phonetic, string? Example);
    public record CreateDeckReq(
        string SubjectCode, string Name, string PayloadType,
        string? Description, List<ItemReq>? Items);
    public record UpdateDeckReq(string Name, string? Description);
    public record UpdateItemReq(string Front, string Back, string? Phonetic, string? Example);
    public record AddItemsReq(List<ItemReq> Items);

    public record DeckDto(
        Guid Id, string Code, string Name, string PayloadType, string Description,
        string SubjectCode, int ItemCount, DateTime UpdatedAt, bool IsShared = false);

    /// <summary>发现页条目（他人已分享卡组，含分享者展示名）</summary>
    public record SharedDeckDto(
        Guid Id, string Name, string PayloadType, string Description,
        string SubjectCode, int ItemCount, string OwnerName, DateTime? SharedAt);

    public record ShareReq(bool Shared);
    public record ForkReq(string? Name);
    public record ItemDto(Guid Id, int Order, string Front, string Back, string Phonetic, string Example);
    public record SubjectDto(string Code, string Name, int SortOrder);

    // ---- 查询 ----

    /// <summary>我的卡组列表（含条目数）</summary>
    [HttpGet("decks")]
    public async Task<IActionResult> List(CancellationToken ct)
    {
        var ownerId = OwnerId;
        var decks = await _uow.Db.Decks.AsNoTracking()
            .Where(d => d.OwnerId == ownerId)
            .OrderByDescending(d => d.UpdatedAt ?? d.CreatedAt)
            .ToListAsync(ct);
        var subjects = await _uow.Db.Subjects.AsNoTracking()
            .ToDictionaryAsync(s => s.Id, s => s.Code, ct);

        var counts = await _uow.Db.Words.AsNoTracking()
            .Where(w => w.DeckId != null && !w.Archived)
            .GroupBy(w => w.DeckId!.Value)
            .ToDictionaryAsync(g => g.Key, g => g.Count(), ct);

        var dto = decks.Select(d => new DeckDto(
            d.Id, d.Code, d.Name, d.PayloadType, d.Description,
            subjects.GetValueOrDefault(d.SubjectId, "en"),
            counts.GetValueOrDefault(d.Id, 0),
            d.UpdatedAt ?? d.CreatedAt, d.IsShared)).ToList();
        return Ok(ApiResponse<List<DeckDto>>.Ok(dto));
    }

    /// <summary>科目清单（编辑器新建下拉；公共数据）</summary>
    [HttpGet("subjects")]
    public async Task<IActionResult> Subjects(CancellationToken ct)
    {
        var list = await _uow.Db.Subjects.AsNoTracking()
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

        var items = await _uow.Db.Words.AsNoTracking()
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

        var subject = await _uow.Db.Subjects.AsNoTracking()
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
        _uow.Db.Decks.Add(deck);

        if (req.Items is { Count: > 0 })
        {
            int v = await _uow.Db.Words.AsNoTracking()
                .MaxAsync(w => (int?)w.Version, ct) ?? 0;
            foreach (var item in req.Items)
                _uow.Db.Words.Add(ToWord(deck, item, ++v));
        }
        await _uow.Db.SaveChangesAsync(ct);

        return Ok(ApiResponse<DeckDto>.Ok(new DeckDto(
            deck.Id, deck.Code, deck.Name, deck.PayloadType, deck.Description,
            subject.Code, req.Items?.Count ?? 0, deck.CreatedAt, deck.IsShared)));
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
        await _uow.Db.SaveChangesAsync(ct);
        return Ok(ApiResponse<object>.Ok(new { deck.Id }));
    }

    /// <summary>删除卡组（Deck 软删 + 条目全部归档，Version++ 使设备
    /// 增量同步感知；LAN 落 SD 的拷贝独立，App 可另行重推清空）</summary>
    [HttpDelete("decks/{id}")]
    public async Task<IActionResult> Delete(Guid id, CancellationToken ct)
    {
        var deck = await FindOwned(id, ct);
        if (deck == null) return NotFound(ApiResponse.Fail(404, "卡组不存在"));

        int v = await _uow.Db.Words.AsNoTracking()
            .MaxAsync(w => (int?)w.Version, ct) ?? 0;
        var items = await _uow.Db.Words.Where(w => w.DeckId == id && !w.Archived).ToListAsync(ct);
        foreach (var w in items)
        {
            w.Archived = true;
            w.Version = ++v;
        }
        // 软删（RepositoryBase.DeleteAsync 同语义：IsDeleted + UpdatedAt，
        // 全局过滤器隐藏；此处裸 DbContext 直接置位）
        deck.IsDeleted = true;
        deck.UpdatedAt = DateTime.UtcNow;
        await _uow.Db.SaveChangesAsync(ct);
        return Ok(ApiResponse<object>.Ok(new { deck.Id }));
    }

    // ---- UGC 分享（v2.0 #3 生态首增量） ----

    /// <summary>分享开关（仅归属人）。开启置 SharedAt（发现页排序），
    /// 关闭置 null；已导入副本不受影响。</summary>
    [HttpPost("decks/{id}/share")]
    public async Task<IActionResult> Share(Guid id, [FromBody] ShareReq req, CancellationToken ct)
    {
        var deck = await FindOwned(id, ct);
        if (deck == null) return NotFound(ApiResponse.Fail(404, "卡组不存在"));

        deck.IsShared = req.Shared;
        deck.SharedAt = req.Shared ? DateTime.UtcNow : null;
        await _uow.Db.SaveChangesAsync(ct);
        return Ok(ApiResponse<object>.Ok(new { deck.Id, deck.IsShared }));
    }

    /// <summary>发现页：他人已分享卡组（SharedAt 倒序）。q=名称包含
    /// （不分大小写）、subject=科目 Code 过滤、page/pageSize 分页。</summary>
    [HttpGet("decks/shared")]
    public async Task<IActionResult> Shared(
        [FromQuery] int page = 1, [FromQuery] int pageSize = 20,
        [FromQuery] string? q = null, [FromQuery] string? subject = null,
        CancellationToken ct = default)
    {
        if (page < 1) page = 1;
        if (pageSize is < 1 or > 50) pageSize = 20;

        var query = _uow.Db.Decks.AsNoTracking()
            .Where(d => d.IsShared && d.OwnerId != OwnerId);
        if (!string.IsNullOrWhiteSpace(q))
        {
            var kw = q.Trim().ToLowerInvariant();
            query = query.Where(d => d.Name.ToLower().Contains(kw));
        }
        if (!string.IsNullOrWhiteSpace(subject))
        {
            var subjectId = await _uow.Db.Subjects.AsNoTracking()
                .Where(s => s.Code == subject)
                .Select(s => (Guid?)s.Id)
                .FirstOrDefaultAsync(ct);
            if (subjectId == null)
                return Ok(ApiResponse<List<SharedDeckDto>>.Ok([]));
            query = query.Where(d => d.SubjectId == subjectId.Value);
        }

        var page_ = await query
            .OrderByDescending(d => d.SharedAt)
            .Skip((page - 1) * pageSize).Take(pageSize + 1) // +1 探测下一页
            .ToListAsync(ct);
        var hasMore = page_.Count > pageSize;
        if (hasMore) page_.RemoveAt(page_.Count - 1);

        var subjects = await _uow.Db.Subjects.AsNoTracking()
            .ToDictionaryAsync(s => s.Id, s => s.Code, ct);
        var ownerIds = page_.Select(d => d.OwnerId!.Value).Distinct().ToList();
        var owners = await _uow.Db.Accounts.AsNoTracking()
            .Where(a => ownerIds.Contains(a.Id))
            .ToDictionaryAsync(a => a.Id, a => a.DisplayName, ct);
        var deckIds = page_.Select(d => d.Id).ToList();
        var counts = await _uow.Db.Words.AsNoTracking()
            .Where(w => w.DeckId != null && !w.Archived && deckIds.Contains(w.DeckId.Value))
            .GroupBy(w => w.DeckId!.Value)
            .ToDictionaryAsync(g => g.Key, g => g.Count(), ct);

        var dto = page_.Select(d => new SharedDeckDto(
            d.Id, d.Name, d.PayloadType, d.Description,
            subjects.GetValueOrDefault(d.SubjectId, "en"),
            counts.GetValueOrDefault(d.Id, 0),
            owners.GetValueOrDefault(d.OwnerId!.Value, ""), d.SharedAt)).ToList();
        return Ok(ApiResponse<object>.Ok(new { items = dto, hasMore }));
    }

    /// <summary>导入（fork 深拷贝）：官方卡组或他人已分享卡组 → 独立
    /// 副本（新 Code + 条目全量拷贝，Version 接全局 max 递增——增量
    /// 同步通道语义同 AddItems）。自己已拥有的不可导入；他人未分享的
    /// 不存在（404 防存在性探测）。</summary>
    [HttpPost("decks/{id}/fork")]
    public async Task<IActionResult> Fork(Guid id, [FromBody] ForkReq? req, CancellationToken ct)
    {
        var deck = await _uow.Db.Decks.AsNoTracking()
            .FirstOrDefaultAsync(d => d.Id == id, ct);
        if (deck == null || (deck.OwnerId != null && !deck.IsShared))
            return NotFound(ApiResponse.Fail(404, "卡组不存在或未分享"));
        if (deck.OwnerId == OwnerId)
            return BadRequest(ApiResponse.Fail(400, "自己的卡组无需导入"));

        var name = string.IsNullOrWhiteSpace(req?.Name)
            ? deck.Name : req!.Name!.Trim();
        if (name.Length is < 1 or > 128)
            return BadRequest(ApiResponse.Fail(400, "卡组名 1~128 字符"));

        var copy = new InkWord.Core.Entities.Deck
        {
            SubjectId = deck.SubjectId,
            Code = await NextCodeAsync(deck.SubjectId, ct),
            Name = name,
            PayloadType = deck.PayloadType,
            Description = deck.Description,
            OwnerId = OwnerId,
        };
        _uow.Db.Decks.Add(copy);

        var items = await _uow.Db.Words.AsNoTracking()
            .Where(w => w.DeckId == id && !w.Archived)
            .OrderBy(w => w.Version)
            .ToListAsync(ct);
        int v = await _uow.Db.Words.AsNoTracking()
            .MaxAsync(w => (int?)w.Version, ct) ?? 0;
        foreach (var w in items)
            _uow.Db.Words.Add(CopyWord(w, copy, ++v));
        await _uow.Db.SaveChangesAsync(ct);

        return Ok(ApiResponse<DeckDto>.Ok(new DeckDto(
            copy.Id, copy.Code, copy.Name, copy.PayloadType, copy.Description,
            _uow.Db.Subjects.AsNoTracking()
                .Where(s => s.Id == copy.SubjectId)
                .Select(s => s.Code)
                .First(),
            items.Count, copy.CreatedAt, copy.IsShared)));
    }

    /// <summary>fork 深拷贝映射（public static 供测试直测）：内容字段
    /// 全量拷贝（PayloadJson/Audio 等同源可用），归属重定向（Tag=新
    /// Code、SubjectId/DeckId=新卡组）；AiStatus/AiSuggestion 不拷——
    /// 源卡组私有审校状态对新副本无意义；ChangeType 归 0（新增语义）。</summary>
    public static InkWord.Core.Entities.Word CopyWord(
        InkWord.Core.Entities.Word src,
        InkWord.Core.Entities.Deck deck, int version)
    {
        var w = new InkWord.Core.Entities.Word
        {
            Text = src.Text,
            Phonetic = src.Phonetic,
            Meaning = src.Meaning,
            Example = src.Example,
            Audio = src.Audio,
            Tag = deck.Code,
            Root = src.Root,
            Inflections = src.Inflections,
            Source = src.Source,
            Grade = src.Grade,
            Difficulty = src.Difficulty,
            Version = version,
            ChangeType = 0,
            SubjectId = deck.SubjectId,
            DeckId = deck.Id,
            Front = src.Front,
            Back = src.Back,
            PayloadJson = src.PayloadJson,
        };
        return w;
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

        int v = await _uow.Db.Words.AsNoTracking()
            .MaxAsync(w => (int?)w.Version, ct) ?? 0;
        foreach (var item in req.Items)
            _uow.Db.Words.Add(ToWord(deck, item, ++v));
        await _uow.Db.SaveChangesAsync(ct);
        return Ok(ApiResponse<object>.Ok(new { added = req.Items.Count }));
    }

    /// <summary>改条目（Version 接全局 max 递增供设备增量同步；
    /// 单条写路径与批量 AddItems 同源语义——全局 max 被管理端 CRUD/AI 审核
    /// 推高后，裸 Version++ 会产出 ≤ 设备已拉游标的版本而漏发）</summary>
    [HttpPut("decks/{id}/items/{wordId}")]
    public async Task<IActionResult> UpdateItem(
        Guid id, Guid wordId, [FromBody] UpdateItemReq req, CancellationToken ct)
    {
        var deck = await FindOwned(id, ct);
        if (deck == null) return NotFound(ApiResponse.Fail(404, "卡组不存在"));

        var w = await _uow.Db.Words.FirstOrDefaultAsync(
            x => x.Id == wordId && x.DeckId == id, ct);
        if (w == null) return NotFound(ApiResponse.Fail(404, "条目不存在"));

        FillWord(w, req.Front, req.Back, req.Phonetic, req.Example);
        w.Version = NextVersion(w.Version, await _uow.Db.Words.AsNoTracking()
            .MaxAsync(x => (int?)x.Version, ct) ?? 0);
        await _uow.Db.SaveChangesAsync(ct);
        return Ok(ApiResponse<object>.Ok(new { w.Id }));
    }

    /// <summary>删条目（归档 + Version 接全局 max 递增，同改条目写路径）</summary>
    [HttpDelete("decks/{id}/items/{wordId}")]
    public async Task<IActionResult> DeleteItem(Guid id, Guid wordId, CancellationToken ct)
    {
        var deck = await FindOwned(id, ct);
        if (deck == null) return NotFound(ApiResponse.Fail(404, "卡组不存在"));

        var w = await _uow.Db.Words.FirstOrDefaultAsync(
            x => x.Id == wordId && x.DeckId == id, ct);
        if (w == null) return NotFound(ApiResponse.Fail(404, "条目不存在"));

        w.Archived = true;
        w.Version = NextVersion(w.Version, await _uow.Db.Words.AsNoTracking()
            .MaxAsync(x => (int?)x.Version, ct) ?? 0);
        await _uow.Db.SaveChangesAsync(ct);
        return Ok(ApiResponse<object>.Ok(new { w.Id }));
    }

    // ---- 内部 ----

    private Guid OwnerId =>
        Guid.TryParse(User.FindFirstValue(ClaimTypes.NameIdentifier), out var g)
            ? g
            : throw new UnauthorizedAccessException("token 缺少账户标识");

    /// <summary>归属校验（OwnerId 匹配才可见可改）</summary>
    private Task<InkWord.Core.Entities.Deck?> FindOwned(Guid id, CancellationToken ct) =>
        _uow.Db.Decks.FirstOrDefaultAsync(d => d.Id == id && d.OwnerId == OwnerId, ct);

    /// <summary>Code 生成：Guid N 前 7 位（设备 deck id 约束同源），冲突重试</summary>
    internal async Task<string> NextCodeAsync(Guid subjectId, CancellationToken ct)
    {
        for (var i = 0; i < 5; i++)
        {
            var code = "u" + Guid.NewGuid().ToString("N")[..6];
            if (!await _uow.Db.Decks.AsNoTracking()
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

    /// <summary>Version 接全局 max 递增（设备增量同步下发契约：新 Version
    /// 必须高于任一设备可能已拉取的全局最大游标；P3 管理端条目 CRUD 与
    /// me 端写路径同源共用，禁复制）。public static 供测试直测。
    /// 已知竞态：调用方「读 max → 写」非原子（同 Admin/me 既有模式，
    /// Words.Version 非唯一索引），并发写可铸出相同 Version；深度修复需
    /// 事务锁重读 max 或数据库序列发号，留集成验证补齐。</summary>
    public static int NextVersion(int currentVersion, int globalMax) =>
        Math.Max(currentVersion, globalMax) + 1;
}
