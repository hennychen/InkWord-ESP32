using System.Text.Json;
using Hangfire;
using InkWord.Infrastructure.Repositories;
using InkWord.Services;
using Microsoft.EntityFrameworkCore;
using Microsoft.Extensions.Logging;

namespace InkWord.Jobs;

/// <summary>
/// AI 卡组条目批量生成任务（v1.5 T5.4，2026-08-25）。
///
/// 从素材（课文全文/知识点清单）经 AiContentService.GenerateDeckItemsAsync
/// 生成整副卡组条目建议，落占位 Word 行：Text=Front 题面（满足 uq(Text,Tag)
/// 与 NOT NULL）、Version=0、AiStatus=1 待审。Version=0 使
/// GetIncrementalAsync（Version &gt; cursor，设备初始 cursor≥0）恒不下发——
/// 审校前设备增量同步与全量导出均不可见（防 LLM 幻觉污染词库红线）；
/// AdminWordController ai-apply 审校通过时按版式映射写正字段 +
/// Version=max+1 走既有增量通道（Version++ 下发契约，设备零改动）。
/// 同 Deck 既有 front（含归档）跳过，防驳回重生成撞 uq(Text,Tag)。
/// </summary>
public class DeckGenJob
{
    private static readonly JsonSerializerOptions JsonOpts = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
    };

    private readonly IUnitOfWork _uow;
    private readonly AiContentService _ai;
    private readonly ILogger<DeckGenJob> _logger;

    public DeckGenJob(IUnitOfWork uow, AiContentService ai, ILogger<DeckGenJob> logger)
    {
        _uow = uow;
        _ai = ai;
        _logger = logger;
    }

    /// <summary>deckId 目标卡组；source 素材文本；limit 条数上限（服务内钳制）。
    /// 素材级失败（解析/异常）仅记日志返回——无占位行可标 AiStatus=3。</summary>
    [AutomaticRetry(Attempts = 2)]
    public async Task RunAsync(Guid deckId, string source, int limit, CancellationToken ct)
    {
        var deck = await _uow.Db.Decks.AsNoTracking()
            .FirstOrDefaultAsync(d => d.Id == deckId, ct);
        if (deck == null)
        {
            _logger.LogWarning("DeckGenJob 卡组不存在：{DeckId}", deckId);
            return;
        }

        // 科目显示名进 prompt 占位符（T3.3 参数化链路）；无科目回退英语默认
        var subjectName = await _uow.Db.Subjects.AsNoTracking()
            .Where(s => s.Id == deck.SubjectId)
            .Select(s => s.Name)
            .FirstOrDefaultAsync(ct);

        var items = await _ai.GenerateDeckItemsAsync(deck, subjectName, source, limit, ct);
        if (items is null || items.Count == 0)
        {
            _logger.LogWarning("DeckGenJob 生成失败/空：deck={Code}", deck.Code);
            return;
        }

        // 既有 front（含归档，占位行驳回后 Text 仍在）：同题面不重建
        var existing = (await _uow.Db.Words.AsNoTracking()
                .Where(w => w.DeckId == deck.Id)
                .Select(w => w.Front)
                .ToListAsync(ct))
            .ToHashSet(StringComparer.Ordinal);

        var created = 0;
        foreach (var sug in items)
        {
            if (sug.Front is null || existing.Contains(sug.Front)) continue;
            existing.Add(sug.Front);
            _uow.Db.Words.Add(new InkWord.Core.Entities.Word
            {
                Text = sug.Front,
                Front = sug.Front,
                Tag = deck.Code,
                SubjectId = deck.SubjectId,
                DeckId = deck.Id,
                Version = 0, // 占位行不下发（审校通过时 ai-apply 才 Version=max+1）
                ChangeType = 0,
                AiStatus = 1,
                AiSuggestion = JsonSerializer.Serialize(sug, JsonOpts),
            });
            created++;
        }
        await _uow.Db.SaveChangesAsync(ct);
        _logger.LogInformation(
            "DeckGenJob deck={Code} 占位待审 {Created} 条（建议 {Total} 条，跳过重复 {Skipped} 条）",
            deck.Code, created, items.Count, items.Count - created);
    }
}
