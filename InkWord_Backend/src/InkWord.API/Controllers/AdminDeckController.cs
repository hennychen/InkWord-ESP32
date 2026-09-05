using System.Text;
using Hangfire;
using InkWord.API.DTOs;
using InkWord.Core.Common;
using InkWord.Infrastructure.Repositories;
using InkWord.Jobs;
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Mvc;
using Microsoft.EntityFrameworkCore;

namespace InkWord.API.Controllers;

/// <summary>
/// 管理端卡组接口（v1.4 T4.5 字库子集下发；v1.5 T5.4 AI 卡组生成）。
/// </summary>
[ApiController]
[Route("api/admin/decks")]
[Authorize(Roles = "Admin,Operator")]
public class AdminDeckController : ControllerBase
{
    private readonly IUnitOfWork _uow;

    public AdminDeckController(IUnitOfWork uow) => _uow = uow;

    /// <summary>卡组列表（T5.4：AI 生成弹窗下拉数据源；含条目计数）</summary>
    [HttpGet]
    public async Task<IActionResult> List(CancellationToken ct)
    {
        var decks = await _uow.Db.Decks.AsNoTracking()
            .Where(d => !d.IsDeleted)
            .OrderBy(d => d.Code)
            .Select(d => new
            {
                d.Id, d.Code, d.Name, d.PayloadType, d.SubjectId,
                ItemCount = _uow.Db.Words.Count(w => w.DeckId == d.Id && !w.Archived),
            })
            .ToListAsync(ct);
        return Ok(ApiResponse<object>.Ok(decks));
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
}
