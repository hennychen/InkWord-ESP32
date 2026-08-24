using Hangfire;
using InkWord.Infrastructure.DbContext;
using InkWord.Services;
using Microsoft.EntityFrameworkCore;
using Microsoft.Extensions.Logging;

namespace InkWord.Jobs;

/// <summary>
/// 词条 TTS 批量合成任务（P0B，2026-08-24）。
///
/// 扫描「未归档且 data/audio/{Id:N}.mp3 缺失」的词条，串行调 Piper 合成
/// （CPU 单核足够，无需并发限流）。文件存在性无法入库查询：仅取 Id 列表
/// （5000 词级内存可忽略），客户端过滤后分页处理。
///
/// 幂等：成功即有文件、次日自然跳过；失败词不标记（piper 未安装时整轮
/// 空转快速结束，AutomaticRetry(0) 避免重试风暴，次日重扫）。
/// 与词库 Version/AiStatus 完全解耦——音频是缓存资产而非词库数据。
/// </summary>
public class TtsJob
{
    private const int PageSize = 50;
    private const int MaxPerRun = 2000;

    private readonly AppDbContext _db;
    private readonly TtsService _tts;
    private readonly ILogger<TtsJob> _logger;

    public TtsJob(AppDbContext db, TtsService tts, ILogger<TtsJob> logger)
    {
        _db = db;
        _tts = tts;
        _logger = logger;
    }

    [AutomaticRetry(Attempts = 0)]
    public async Task RunAsync(int limit, CancellationToken ct)
    {
        limit = limit <= 0 || limit > MaxPerRun ? MaxPerRun : limit;

        // Id 全量轻量拉取 → 文件缺失过滤 → 截断到 limit
        var missing = (await _db.Words.AsNoTracking()
                .Where(w => !w.Archived)
                .Select(w => new { w.Id, w.Text })
                .ToListAsync(ct))
            .Where(w => !_tts.HasAudio(w.Id))
            .Take(limit)
            .ToList();

        if (missing.Count == 0)
        {
            _logger.LogInformation("TtsJob：无缺失音频，跳过");
            return;
        }
        _logger.LogInformation("TtsJob 开始：待合成 {Count} 条", missing.Count);

        var done = 0;
        var failed = 0;
        foreach (var batch in Chunk(missing, PageSize))
        {
            foreach (var w in batch)
            {
                var word = new Core.Entities.Word { Id = w.Id, Text = w.Text };
                if (await _tts.EnsureWordAudioAsync(word, ct)) done++;
                else failed++;
            }
            _logger.LogInformation("TtsJob 批次完成：累计成功 {Done} / 失败 {Failed}", done, failed);
            if (failed >= PageSize * 2 && done == 0)
            {
                // 引擎大概率不可用（piper 未装/模型缺失）：整轮止损
                _logger.LogError("TtsJob 止损：连续 {Failed} 次失败且零成功，疑似引擎不可用", failed);
                return;
            }
        }

        _logger.LogInformation("TtsJob 结束：成功 {Done}，失败 {Failed}", done, failed);
    }

    private static IEnumerable<List<T>> Chunk<T>(List<T> source, int size)
    {
        for (var i = 0; i < source.Count; i += size)
            yield return source.GetRange(i, Math.Min(size, source.Count - i));
    }
}

/// <summary>对话音频清理（P2A，2026-08-24）：每小时回收过期 chat_*.mp3。</summary>
public class ChatAudioCleanupJob
{
    private readonly TtsService _tts;
    private readonly ILogger<ChatAudioCleanupJob> _logger;

    public ChatAudioCleanupJob(TtsService tts, ILogger<ChatAudioCleanupJob> logger)
    {
        _tts = tts;
        _logger = logger;
    }

    public Task RunAsync()
    {
        var removed = _tts.CleanupChatClips(TimeSpan.FromHours(1));
        if (removed > 0)
            _logger.LogInformation("ChatAudioCleanup：回收 {Count} 个过期对话音频", removed);
        return Task.CompletedTask;
    }
}
