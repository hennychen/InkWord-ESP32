using InkWord.Infrastructure.Cache;
using Microsoft.Extensions.AI;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.Logging;

namespace InkWord.Services;

/// <summary>对话轮消息（Redis 会话上下文存储单元，role: user/assistant）</summary>
public record ChatMsg(string Role, string Text);

/// <summary>对话响应（camelCase 下发设备）：转写/回复/引擎/音频地址</summary>
public record ChatReply(string Transcript, string Reply, string Engine, string? AudioUrl);

/// <summary>对话编排失败：StatusCode 对应 HTTP 响应码（端点透传）</summary>
public class ChatException : Exception
{
    public int StatusCode { get; }
    public ChatException(int statusCode, string message) : base(message) => StatusCode = statusCode;
}

/// <summary>
/// 语音对话编排（P2A，2026-08-24）：ASR → IChatClient → TtsService 单端点闭环。
///
/// 会话上下文：Redis chat:{deviceId:N} 存最近 12 轮 messages（user+assistant
/// 各一计 24 条），TTL 30 分钟超时自然新会话；Redis 不可用时降级无上下文
/// 单轮（告警不阻断）。安全边界：英语外教 persona + 儿童安全 system
/// prompt，输出再过一层黑名单（命中替换安全引导语）；长度护栏 ≤2 句
/// ≤40 词（TTS 时长约束）。TTS 失败降级 audioUrl=null——音频是体验
/// 增强而非协议必需，与「音频为缓存资产」红线一致。
/// </summary>
public class ChatService
{
    /// <summary>对话 WAV 硬上限（10s 16kHz/16bit/mono=320KB，512KB 含余量；端点 413 同源）</summary>
    public const int MaxWavBytes = 512 * 1024;

    private const int MaxContextMessages = 24;               // 最近 12 轮
    private static readonly TimeSpan SessionTtl = TimeSpan.FromMinutes(30);
    private const int MaxReplyWords = 40;                    // TTS 时长约束
    private const int MaxReplySentences = 2;
    private const int MaxReplyChars = 260;                   // 字符兜底（TtsService.MaxTextLength=300 内）

    /// <summary>输出黑名单（一期）：命中替换安全引导语（儿童安全边界兜底）</summary>
    private static readonly HashSet<string> Blocklist =
    [
        "kill", "suicide", "naked", "porn", "sex", "drug", "gun", "blood", "hate you",
    ];

    private const string SafeReply =
        "Sorry, let's talk about something fun! What is your favorite animal?";

    private readonly IAsrTranscriber _asr;
    private readonly IChatClient _chat;
    private readonly TtsService _tts;
    private readonly IRedisCache _cache;
    private readonly ILogger<ChatService> _logger;
    private readonly string _engine;

    public ChatService(IAsrTranscriber asr, IChatClient chat, TtsService tts,
        IRedisCache cache, IConfiguration cfg, ILogger<ChatService> logger)
    {
        _asr = asr;
        _chat = chat;
        _tts = tts;
        _cache = cache;
        _logger = logger;
        _engine = cfg["Ai:Model"] ?? "ollama";
    }

    /// <summary>一轮对话：wav(16k/16bit/mono) → { transcript, reply, engine, audioUrl }</summary>
    public async Task<ChatReply> ConverseAsync(Guid deviceId, byte[] wav, CancellationToken ct)
    {
        // 1. WAV → PCM（复用发音评测解析器，上限放宽到对话档）
        short[] samples;
        try
        {
            samples = PronunciationService.ParsePcm(wav, MaxWavBytes);
        }
        catch (InvalidDataException ex)
        {
            throw new ChatException(400, ex.Message);
        }

        // 2. ASR（null=引擎不可用；空串=无话音）
        var transcript = _asr.Transcribe(samples);
        if (transcript is null) throw new ChatException(503, "asr unavailable");
        if (transcript.Length == 0) throw new ChatException(400, "no speech detected");

        // 3. 会话上下文（Redis 挂 → 无上下文单轮降级）
        var cacheKey = $"chat:{deviceId:N}";
        var history = await TryGetHistoryAsync(cacheKey, ct);

        // 4. LLM
        string reply;
        try
        {
            var response = await _chat.GetResponseAsync(
                BuildMessages(history, transcript),
                new ChatOptions { Temperature = 0.8f }, ct);
            reply = response.Text.Trim();
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            _logger.LogWarning(ex, "chat LLM 调用失败（ollama 未起/模型未拉?）");
            throw new ChatException(502, "llm unavailable");
        }
        if (reply.Length == 0) throw new ChatException(502, "empty llm reply");

        // 5. 安全与长度护栏
        reply = ContainsBlocked(reply) ? SafeReply : EnforceLimits(reply);

        // 6. TTS（失败降级 audioUrl=null，文本回复不受影响）
        var fileName = $"chat_{DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()}.mp3";
        var saved = await _tts.SaveClipAsync(fileName, reply, ct);
        if (saved is null)
            _logger.LogWarning("chat TTS 失败，audioUrl 置空：{File}", fileName);

        // 7. 上下文回写（含本轮，裁剪保留最近 12 轮）
        history.Add(new ChatMsg("user", transcript));
        history.Add(new ChatMsg("assistant", reply));
        await TrySaveHistoryAsync(cacheKey, history, ct);

        return new ChatReply(transcript, reply, _engine,
            saved is null ? null : $"/api/device/audio/{saved}");
    }

    // ---- Prompt 与护栏 ----

    private const string SystemPrompt =
        """
        You are a friendly English tutor talking with a young Chinese student on a vocabulary device.
        - Always reply in simple English (A1-A2 words, short sentences).
        - Keep the reply within 2 sentences and 40 words.
        - Be warm, encouraging and a bit playful; ask one small question when it helps.
        - Safe topics: school, hobbies, animals, sports, food, weather, daily life.
        - If the student asks about violence, romance, or anything unsafe for children, gently say no and suggest a safe topic.
        - Never say you are an AI or a model. Never give contact details or links.
        """;

    internal static List<ChatMessage> BuildMessages(IReadOnlyList<ChatMsg> history, string userText)
    {
        var messages = new List<ChatMessage> { new(ChatRole.System, SystemPrompt) };
        foreach (var m in history)
            messages.Add(new ChatMessage(new ChatRole(m.Role), m.Text));
        messages.Add(new ChatMessage(ChatRole.User, userText));
        return messages;
    }

    /// <summary>长度护栏：≤2 句、≤40 词（词边界截断）、字符兜底（公开供测试与复用）</summary>
    public static string EnforceLimits(string reply)
    {
        var parts = reply.Split(new[] { '.', '!', '?', '。', '！', '？' },
            StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length > MaxReplySentences)
            reply = string.Join('.', parts.Take(MaxReplySentences)).TrimEnd() + ".";
        var words = reply.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        if (words.Length > MaxReplyWords)
            reply = string.Join(' ', words.Take(MaxReplyWords - 1)) + "…";
        return reply.Length <= MaxReplyChars ? reply : reply[..MaxReplyChars];
    }

    internal static bool ContainsBlocked(string text)
    {
        foreach (var w in Blocklist)
            if (text.Contains(w, StringComparison.OrdinalIgnoreCase)) return true;
        return false;
    }

    // ---- Redis 上下文（全链路降级：任何故障不阻断对话） ----

    private async Task<List<ChatMsg>> TryGetHistoryAsync(string key, CancellationToken ct)
    {
        try
        {
            return await _cache.GetAsync<List<ChatMsg>>(key, ct) ?? [];
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            _logger.LogWarning(ex, "chat 上下文读取失败，降级无上下文单轮");
            return [];
        }
    }

    private async Task TrySaveHistoryAsync(string key, List<ChatMsg> history, CancellationToken ct)
    {
        if (history.Count > MaxContextMessages)
            history.RemoveRange(0, history.Count - MaxContextMessages);
        try
        {
            await _cache.SetAsync(key, history, SessionTtl, ct);
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            _logger.LogWarning(ex, "chat 上下文回写失败（本轮响应不受影响）");
        }
    }
}
