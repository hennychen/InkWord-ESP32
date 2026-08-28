using System.Text;
using InkWord.Core.Entities;
using InkWord.Infrastructure.DbContext;
using Microsoft.EntityFrameworkCore;
using Microsoft.Extensions.Logging;

namespace InkWord.Services;

/// <summary>语音查词候选（camelCase 下发设备）：text/释义截断/cloudId/层级分</summary>
public record VoiceCandidate(string Text, string Meaning, string CloudId, double Score);

/// <summary>语音查词响应：ASR 转写 + top-5 候选（空列表=无命中）</summary>
public record VoiceSearchResult(string Transcript, List<VoiceCandidate> Candidates);

/// <summary>查词编排失败：StatusCode 对应 HTTP 响应码（端点透传，ChatException 同款）</summary>
public class VoiceSearchException : Exception
{
    public int StatusCode { get; }
    public VoiceSearchException(int statusCode, string message) : base(message) =>
        StatusCode = statusCode;
}

/// <summary>
/// 语音查词编排（2026-08-28，教材目录浏览+语音查词设计 §B1）：
/// WAV → ASR → 词库三级匹配 → top-5 候选。
///
/// 匹配范围优先设备活跃词书（?deck={Code}，失配/缺省兜底全库）；
/// 文本归一（去标点/小写/压缩空格）后分层：L1 精确全等（1.0）→
/// L2 前缀/包含（0.7，短边 ≥3 防冠词误命中）→ L3 编辑距离 ≤2
/// 且词长 ≥5（0.4，兜 ASR 拼写漂移）；同词多路径命中取最高分。
/// Normalize/ScoreWord/EditDistance 纯函数公开供单测。
/// </summary>
public class VoiceSearchService
{
    /// <summary>查词 WAV 硬上限（5s 16kHz/16bit/mono=160KB，256KB 含余量；端点 413 同源）</summary>
    public const int MaxWavBytes = 256 * 1024;
    public const int MaxCandidates = 5;
    private const int MeaningMaxChars = 60;

    private readonly IAsrTranscriber _asr;
    private readonly AppDbContext _db;
    private readonly ILogger<VoiceSearchService> _logger;

    public VoiceSearchService(IAsrTranscriber asr, AppDbContext db,
        ILogger<VoiceSearchService> logger)
    {
        _asr = asr;
        _db = db;
        _logger = logger;
    }

    /// <summary>一轮查词：wav(16k/16bit/mono) + 词书范围 → { transcript, candidates }</summary>
    public async Task<VoiceSearchResult> SearchAsync(byte[] wav, string? deckCode,
        CancellationToken ct)
    {
        // 1. WAV → PCM（复用发音评测解析器，上限放宽到查词档）
        short[] samples;
        try
        {
            samples = PronunciationService.ParsePcm(wav, MaxWavBytes);
        }
        catch (InvalidDataException ex)
        {
            throw new VoiceSearchException(400, ex.Message);
        }

        // 2. ASR（null=引擎不可用；空串=无话音；chat 端点同款语义）
        var transcript = _asr.Transcribe(samples);
        if (transcript is null) throw new VoiceSearchException(503, "asr unavailable");
        if (transcript.Length == 0) throw new VoiceSearchException(400, "no speech detected");

        // 3. 范围：deckCode 对应卡组词（Code 科目内可能重号，取首个）；
        //    失配/缺省兜底全库（归档词恒排除）
        IQueryable<Word> query = _db.Words.AsNoTracking().Where(w => !w.Archived);
        if (!string.IsNullOrWhiteSpace(deckCode))
        {
            var deckId = await _db.Decks.AsNoTracking()
                .Where(d => d.Code == deckCode)
                .Select(d => (Guid?)d.Id)
                .FirstOrDefaultAsync(ct);
            if (deckId.HasValue)
                query = query.Where(w => w.DeckId == deckId.Value);
        }
        var words = await query
            .Select(w => new { w.Text, w.Meaning, w.Id })
            .ToListAsync(ct);

        // 4. 分层打分 → top-5（同分按文本字典序，排序稳定可复现）
        var norm = Normalize(transcript);
        var candidates = words
            .Select(w => (Word: w, Score: ScoreWord(norm, w.Text)))
            .Where(x => x.Score > 0)
            .OrderByDescending(x => x.Score)
            .ThenBy(x => x.Word.Text, StringComparer.OrdinalIgnoreCase)
            .Take(MaxCandidates)
            .Select(x => new VoiceCandidate(
                x.Word.Text, Truncate(x.Word.Meaning, MeaningMaxChars),
                x.Word.Id.ToString(), x.Score))
            .ToList();

        _logger.LogInformation(
            "voice-search: transcript='{Transcript}' deck={Deck} hits={Hits}/{Total}",
            transcript, deckCode ?? "-", candidates.Count, words.Count);

        return new VoiceSearchResult(transcript, candidates);
    }

    // ---- 纯函数（单测口径） ----

    /// <summary>文本归一：小写、剔除非字母数字、连续空白压单空格</summary>
    public static string Normalize(string text)
    {
        var sb = new StringBuilder(text.Length);
        var pendingSpace = false;
        foreach (var c in text.ToLowerInvariant())
        {
            if (char.IsLetterOrDigit(c))
            {
                if (pendingSpace && sb.Length > 0) sb.Append(' ');
                sb.Append(c);
                pendingSpace = false;
            }
            else
            {
                pendingSpace = true;
            }
        }
        return sb.ToString().Trim();
    }

    /// <summary>
    /// 分层匹配打分：0=不命中；1.0 精确；0.7 前缀/包含（双向，短边 ≥3）；
    /// 0.4 编辑距离 ≤2（词长 ≥5 且长度差 ≤2，防短词误命中）。
    /// </summary>
    public static double ScoreWord(string normTranscript, string wordText)
    {
        var w = Normalize(wordText);
        if (normTranscript.Length == 0 || w.Length == 0) return 0;
        if (w == normTranscript) return 1.0;

        var shorter = Math.Min(w.Length, normTranscript.Length);
        if (shorter >= 3 &&
            (w.Contains(normTranscript, StringComparison.Ordinal) ||
             normTranscript.Contains(w, StringComparison.Ordinal)))
            return 0.7;

        if (w.Length >= 5 && Math.Abs(w.Length - normTranscript.Length) <= 2 &&
            EditDistance(w, normTranscript) <= 2)
            return 0.4;

        return 0;
    }

    /// <summary>Levenshtein 编辑距离（两行 DP；两串均已归一）</summary>
    public static int EditDistance(string a, string b)
    {
        if (a == b) return 0;
        if (a.Length == 0) return b.Length;
        if (b.Length == 0) return a.Length;

        var prev = new int[b.Length + 1];
        var curr = new int[b.Length + 1];
        for (var j = 0; j <= b.Length; j++) prev[j] = j;

        for (var i = 1; i <= a.Length; i++)
        {
            curr[0] = i;
            for (var j = 1; j <= b.Length; j++)
            {
                var cost = a[i - 1] == b[j - 1] ? 0 : 1;
                curr[j] = Math.Min(Math.Min(curr[j - 1] + 1, prev[j] + 1),
                    prev[j - 1] + cost);
            }
            (prev, curr) = (curr, prev);
        }
        return prev[b.Length];
    }

    private static string Truncate(string s, int max) =>
        s.Length <= max ? s : s[..max];
}
