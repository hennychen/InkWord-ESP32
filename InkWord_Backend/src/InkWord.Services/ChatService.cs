using System.Text;
using InkWord.Core.Entities;
using InkWord.Infrastructure.Cache;
using Microsoft.Extensions.AI;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.Logging;

namespace InkWord.Services;

/// <summary>对话轮消息（Redis 会话上下文存储单元，role: user/assistant）</summary>
public record ChatMsg(string Role, string Text);

/// <summary>对话响应（camelCase 下发设备）：转写/回复/引擎/音频地址；
/// Mode/Warmup 为 A1 模式扩展、Lang 为 A2 音色路由（"en"|"zh"，指示
/// 回复主语言）、WordHits 为 A3 生词命中（词库交集，SET 键收藏；free
/// 模式 null 可缺省，老固件忽略新字段）</summary>
public record ChatReply(string Transcript, string Reply, string Engine, string? AudioUrl,
    string? Mode = null, string? Warmup = null, string? Lang = null,
    IReadOnlyList<ChatWordHit>? WordHits = null);

/// <summary>对话生词命中（A3）：text=词库原词面，cloudId=Word.Id（设备
/// SET 键收藏直接复用 /sync/collect 端点，零新协议）</summary>
public record ChatWordHit(string Text, string CloudId);

/// <summary>词库快照条目（A3 wordHits 扫描源）</summary>
public record VocabEntry(string Text, string CloudId);

/// <summary>对话轮异步落库出口（A3）：实现方承担 fire-and-forget 语义
/// （Hangfire Enqueue），失败仅日志不影响对话响应；测试注入 null 即关闭</summary>
public interface IChatTurnSink
{
    void Enqueue(ChatTurn turn);
}

/// <summary>词库快照提供者（A3 wordHits）：实现方负责缓存（对话每轮
/// 查询，全库 5000 词级需 MemoryCache 节流）；null 注入即关闭命中</summary>
public interface IWordListProvider
{
    ValueTask<IReadOnlyList<VocabEntry>> GetWordsAsync(CancellationToken ct);
}

/// <summary>对话编排失败：StatusCode 对应 HTTP 响应码（端点透传）</summary>
public class ChatException : Exception
{
    public int StatusCode { get; }
    public ChatException(int statusCode, string message) : base(message) => StatusCode = statusCode;
}

/// <summary>
/// 语音对话编排（P2A，2026-08-24；A1 模式参数化 2026-08-28）：
/// ASR → IChatClient → TtsService 单端点闭环，free/scenario/translate
/// 三模式共用七步编排（同构，仅 prompt/上下文键/Temperature/护栏四处差异）。
///
/// 会话上下文：Redis 最近 12 轮 messages（user+assistant 各一计 24 条），
/// TTL 30 分钟超时自然新会话；free 键 chat:{deviceId}（现状不变，老会话
/// 无缝续用），scenario=chat:{deviceId}:s:{scenarioId}、translate=
/// chat:{deviceId}:t 按模式隔离；Redis 不可用时降级无上下文单轮
/// （告警不阻断）。安全边界：英语外教 persona + 儿童安全 system
/// prompt，输出再过一层黑名单（命中替换安全引导语，全模式共用）；
/// 长度护栏按模式分档且 UTF-8 字节安全截断（防设备端半字乱码）。
/// TTS 失败降级 audioUrl=null——音频是体验增强而非协议必需，
/// 与「音频为缓存资产」红线一致。
/// </summary>
public class ChatService
{
    /// <summary>对话 WAV 硬上限（10s 16kHz/16bit/mono=320KB，512KB 含余量；端点 413 同源）</summary>
    public const int MaxWavBytes = 512 * 1024;

    /// <summary>对话模式合法值（A1）：free 自由外教 / scenario 场景剧本 / translate 翻译</summary>
    public const string ModeFree = "free";
    public const string ModeScenario = "scenario";
    public const string ModeTranslate = "translate";

    private const int MaxContextMessages = 24;               // 最近 12 轮
    private static readonly TimeSpan SessionTtl = TimeSpan.FromMinutes(30);
    private const int MaxReplyWords = 40;                    // TTS 时长约束（free/scenario 英文档）
    private const int MaxReplySentences = 2;
    private const int MaxReplyChars = 260;                   // 字符兑底（TtsService.MaxTextLength=300 内）

    /// <summary>translate 档回复 UTF-8 字节上限（固件 CHAT_REPLY_MAX=256B 红线内含余量，
    /// 双语两行：英文 ≤25 词 + 中文翻译）</summary>
    public const int TranslateMaxBytes = 240;

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
    private readonly IChatTurnSink? _turnSink;
    private readonly IWordListProvider? _vocab;

    public ChatService(IAsrTranscriber asr, IChatClient chat, TtsService tts,
        IRedisCache cache, IConfiguration cfg, ILogger<ChatService> logger,
        IChatTurnSink? turnSink = null, IWordListProvider? vocab = null)
    {
        _asr = asr;
        _chat = chat;
        _tts = tts;
        _cache = cache;
        _logger = logger;
        _engine = cfg["Ai:Model"] ?? "ollama";
        _turnSink = turnSink;
        _vocab = vocab;
    }

    /// <summary>一轮对话（free 兼容包装：老调用点/现有测试零改动）</summary>
    public Task<ChatReply> ConverseAsync(Guid deviceId, byte[] wav, CancellationToken ct) =>
        ConverseAsync(deviceId, wav, null, null, ct);

    /// <summary>一轮对话（A1 模式参数化）：wav(16k/16bit/mono) + mode/scenarioId
    /// → { transcript, reply, engine, audioUrl, mode, warmup }</summary>
    public async Task<ChatReply> ConverseAsync(Guid deviceId, byte[] wav,
        string? mode, string? scenarioId, CancellationToken ct)
    {
        // 0. 模式解析（null/空白=free——老固件不带 query 行为与现状一致；
        //    非法 mode / scenario 模式缺剧本 → 400）
        var (cfg, script, normMode) = ResolveMode(mode, scenarioId);

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

        // 3. 会话上下文（free 键保持现状；scenario/translate 后缀隔离。
        //    Redis 挂 → 无上下文单轮降级）
        var cacheKey = $"chat:{deviceId:N}{cfg.ContextSuffix}";
        var history = await TryGetHistoryAsync(cacheKey, ct);

        // scenario 首轮：开场白作为 assistant 前缀注入（LLM 知道对话已开始，
        // 不走 LLM 固定文案防人设漂移）；Warmup 携带中文预热供设备屏显
        string? warmup = null;
        if (script is not null && history.Count == 0)
        {
            history.Add(new ChatMsg("assistant", script.OpeningLine));
            warmup = script.WarmupIntro;
        }

        // 4. LLM（Temperature 按模式：translate 0.4 准确性优先，对齐 DeckGen 先例）
        string reply;
        try
        {
            var response = await _chat.GetResponseAsync(
                BuildMessages(history, transcript, cfg.SystemPrompt),
                new ChatOptions { Temperature = cfg.Temperature }, ct);
            reply = response.Text.Trim();
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            _logger.LogWarning(ex, "chat LLM 调用失败（ollama 未起/模型未拉?）");
            throw new ChatException(502, "llm unavailable");
        }
        if (reply.Length == 0) throw new ChatException(502, "empty llm reply");

        // 5. 安全与长度护栏（emoji 剥离+黑名单全模式；长度档随模式）
        reply = StripEmoji(reply);
        reply = ContainsBlocked(reply) ? SafeReply
            : EnforceLimits(reply, cfg.MaxSentences, cfg.MaxWords, cfg.MaxBytes);

        // 5.5 生词命中（A3）：transcript/reply 与词库 L1 精确匹配（可注入
        //    依赖 null=关闭；词库快照由实现方缓存；token 级匹配，短语词
        //    一期不命中）；Redis 降级路径照常命中（只读词库不涉降级）
        IReadOnlyList<ChatWordHit>? wordHits = null;
        if (_vocab is not null)
        {
            var vocab = await _vocab.GetWordsAsync(ct);
            wordHits = MatchWordHits($"{transcript} {reply}", vocab);
        }

        // 6. TTS（失败降级 audioUrl=null，文本回复不受影响）。A2 语言路由：
        //    translate 送首个非中文行（两行格式的英文行，LLM 未按格式全
        //    中文则跳过合成纯屏显）；其余模式按合成文本主语言路由——free
        //    模式学生说中文时 LLM 可能适应回中文，zh 音色播报（外教默认
        //    仍英语）；scenario prompt 锁英语，检测为防御性
        var ttsText = normMode == ModeTranslate ? FirstNonChineseLine(reply) : reply;
        var lang = ttsText is not null && ContainsCjk(ttsText) ? "zh" : "en";
        string? saved = null;
        if (!string.IsNullOrEmpty(ttsText))
        {
            var fileName = $"chat_{DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()}.mp3";
            saved = await _tts.SaveClipAsync(fileName, ttsText, lang, ct);
            if (saved is null)
                _logger.LogWarning("chat TTS 失败，audioUrl 置空：{File}", fileName);
        }

        // 7. 上下文回写（含本轮，裁剪保留最近 12 轮）
        history.Add(new ChatMsg("user", transcript));
        history.Add(new ChatMsg("assistant", reply));
        await TrySaveHistoryAsync(cacheKey, history, ct);

        // 8. 异步落库（A3）：Hangfire fire-and-forget，对话响应不等待；
        //    失败仅日志（转录数据优先留存，Redis 降级不联动关闭）
        _turnSink?.Enqueue(new ChatTurn
        {
            DeviceId = deviceId,
            Mode = normMode,
            ScenarioId = script?.Id,
            Transcript = transcript,
            Reply = reply,
            Ts = DateTime.UtcNow,
        });

        return new ChatReply(transcript, reply, _engine,
            saved is null ? null : $"/api/device/audio/{saved}", normMode, warmup, lang, wordHits);
    }

    // ---- 模式配置（A1 参数化：七步编排同构，仅四处差异） ----

    /// <summary>模式差异化参数：systemPrompt / 上下文键后缀 / Temperature / 护栏档</summary>
    private sealed record ChatModeConfig(string SystemPrompt, string ContextSuffix,
        float Temperature, int MaxSentences, int MaxWords, int MaxBytes);

    /// <summary>模式与剧本解析：非法 mode / scenario 缺剧本 400；
    /// 返回 (配置, 剧本[仅 scenario], 归一化 mode)</summary>
    private static (ChatModeConfig, ScenarioScript?, string) ResolveMode(
        string? mode, string? scenarioId)
    {
        var m = string.IsNullOrWhiteSpace(mode) ? ModeFree : mode.Trim().ToLowerInvariant();
        switch (m)
        {
            case ModeFree:
                return (FreeConfig, null, ModeFree);
            case ModeScenario:
            {
                if (string.IsNullOrWhiteSpace(scenarioId))
                    throw new ChatException(400, "scenarioId required");
                var script = ScenarioLibrary.Get(scenarioId.Trim());
                if (script is null)
                    throw new ChatException(400, "unknown scenario");
                return (ScenarioConfig(script), script, ModeScenario);
            }
            case ModeTranslate:
                return (TranslateConfig, null, ModeTranslate);
            default:
                throw new ChatException(400, "invalid mode");
        }
    }

    /// <summary>英语外教基线（free 档；scenario 档在此之上叠加人设与场景）</summary>
    private const string TutorSystemPrompt =
        """
        You are a friendly English tutor talking with a young Chinese student on a vocabulary device.
        - Always reply in simple English (A1-A2 words, short sentences).
        - Keep the reply within 2 sentences and 40 words.
        - Be warm, encouraging and a bit playful; ask one small question when it helps.
        - Safe topics: school, hobbies, animals, sports, food, weather, daily life.
        - If the student asks about violence, romance, or anything unsafe for children, gently say no and suggest a safe topic.
        - Never say you are an AI or a model. Never give contact details or links.
        """;

    private static ChatModeConfig FreeConfig => new(TutorSystemPrompt, "", 0.8f,
        MaxReplySentences, MaxReplyWords, MaxReplyChars);

    /// <summary>scenario 档：外教基线 + 场景人设/设定/目标词，键后缀 :s:{id}</summary>
    private static ChatModeConfig ScenarioConfig(ScenarioScript s) => new(
        $"""
        {TutorSystemPrompt}
        Now you are {s.Persona}. Scene: {s.SceneSetup}
        Stay in this scene for the whole conversation. Gently guide the student
        to use these words when it fits: {string.Join(", ", s.TargetWords)}.
        """,
        $":s:{s.Id}", 0.7f, MaxReplySentences, MaxReplyWords, MaxReplyChars);

    /// <summary>translate 档（A2 双向互译：自动检测输入语言——说英语纠错
    /// 译中、说中文译英；双语 ASR 就绪后中→英闭环自动成立，qwen 原生
    /// 能力零新模型）：两行格式（英文句/中文翻译），键后缀 :t</summary>
    private static ChatModeConfig TranslateConfig => new(
        """
        You are a translation coach for a young Chinese student on a vocabulary device.
        - The student says ONE sentence in either English or Chinese.
        - Detect the language and translate to the other: English input gets gently
          corrected; Chinese input is translated into English.
        - Reply in EXACTLY two lines: line 1 = the natural English sentence
          (at most 25 words); line 2 = the Chinese translation of line 1.
        - If the English is already perfect, line 1 repeats it unchanged.
        - Never add a third line. Keep everything short and simple (A1-A2 words).
        """,
        ":t", 0.4f, 6, 200, TranslateMaxBytes);

    internal static List<ChatMessage> BuildMessages(IReadOnlyList<ChatMsg> history,
        string userText, string systemPrompt)
    {
        var messages = new List<ChatMessage> { new(ChatRole.System, systemPrompt) };
        foreach (var m in history)
            messages.Add(new ChatMessage(new ChatRole(m.Role), m.Text));
        messages.Add(new ChatMessage(ChatRole.User, userText));
        return messages;
    }

    /// <summary>长度护栏（free/scenario 英文档包装，公开供测试与复用）</summary>
    public static string EnforceLimits(string reply) =>
        EnforceLimits(reply, MaxReplySentences, MaxReplyWords, MaxReplyChars);
    
    /// <summary>长度护栏（模式分档）：≤maxSentences 句、≤maxWords 词（词边界截断）、
    /// UTF-8 字节兑底（rune 边界安全截断，防设备端 strncpy 半字乱码）</summary>
    public static string EnforceLimits(string reply, int maxSentences, int maxWords, int maxBytes)
    {
        var parts = reply.Split(new[] { '.', '!', '?', '。', '！', '？' },
            StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length > maxSentences)
            reply = string.Join('.', parts.Take(maxSentences)).TrimEnd() + ".";
        var words = reply.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        if (words.Length > maxWords)
            reply = string.Join(' ', words.Take(maxWords - 1)) + "…";
        return TruncateUtf8(reply, maxBytes);
    }
    
    /// <summary>UTF-8 字节上限截断（rune 边界安全；英文 ASCII 下与字符截断等价）</summary>
    public static string TruncateUtf8(string text, int maxBytes)
    {
        if (Encoding.UTF8.GetByteCount(text) <= maxBytes) return text;
        var sb = new StringBuilder();
        var used = 0;
        foreach (var rune in text.EnumerateRunes())
        {
            var size = rune.Utf8SequenceLength;
            if (used + size > maxBytes) break;
            sb.Append(rune.ToString());
            used += size;
        }
        return sb.ToString();
    }

    /// <summary>emoji 剥离（屏显红线，2026-08-29 三云实测发现）：qwen 等
    /// 云 LLM 会输出 emoji，墨水屏字体无字形渲染豆腐块。rune 过滤 emoji
    /// 区（U+1F000-1FAFF/U+2600-27BF/U+2B00-2BFF）与零宽连接符
    /// （VS16/ZWJ），ASCII/CJK/中文标点原样保留；剥离后收敛连续空格</summary>
    public static string StripEmoji(string text)
    {
        var sb = new StringBuilder(text.Length);
        var stripped = false;
        foreach (var rune in text.EnumerateRunes())
        {
            var v = rune.Value;
            if (v is (>= 0x1F000 and <= 0x1FAFF) or (>= 0x2600 and <= 0x27BF)
                    or (>= 0x2B00 and <= 0x2BFF) or 0xFE0F or 0x200D)
            {
                stripped = true;
                continue;
            }
            sb.Append(rune.ToString());
        }
        if (!stripped) return text;
        return string.Join(' ', sb.ToString()
            .Split(' ', StringSplitOptions.RemoveEmptyEntries)).TrimEnd();
    }
    
    /// <summary>translate 模式 TTS 文本提取：首个非中文主导行（两行格式的英文行）；
    /// 全部行含中文则 null（audioUrl 降级，双语全屏显）。公开供测试</summary>
    public static string? FirstNonChineseLine(string reply)
    {
        foreach (var line in reply.Split('\n'))
        {
            var t = line.Trim();
            if (t.Length == 0) continue;
            if (!ContainsCjk(t)) return t;
        }
        return null;
    }
    
    private static bool ContainsCjk(string text)
    {
        foreach (var rune in text.EnumerateRunes())
            if (rune.Value is >= 0x4E00 and <= 0x9FFF) return true;
        return false;
    }

    /// <summary>生词命中上限（A3）：对齐语音查词 MaxCandidates 先例，
    /// 控制设备屏显与响应体积</summary>
    public const int MaxWordHits = 5;

    /// <summary>生词命中（A3）：文本分词归一（小写剔标点）→ 与词库归一词
    /// 面 L1 全等交集；保留词库原词面与 cloudId，按首次出现序取前 N。
    /// 公开供单测（VoiceSearchService 纯函数同款惯例）</summary>
    public static List<ChatWordHit> MatchWordHits(string text,
        IReadOnlyList<VocabEntry> vocab)
    {
        // 词库侧归一索引（文本小写剔非字母后为键；空键跳过）
        var index = new Dictionary<string, ChatWordHit>();
        foreach (var v in vocab)
        {
            var key = NormalizeToken(v.Text);
            if (key.Length > 0 && !index.ContainsKey(key))
                index[key] = new ChatWordHit(v.Text, v.CloudId);
        }

        var hits = new List<ChatWordHit>();
        var seen = new HashSet<string>();
        foreach (var raw in text.Split((char[]?)null, StringSplitOptions
                     .RemoveEmptyEntries))
        {
            var token = NormalizeToken(raw);
            if (token.Length == 0 || !seen.Add(token)) continue;
            if (index.TryGetValue(token, out var hit))
            {
                hits.Add(hit);
                if (hits.Count >= MaxWordHits) break;
            }
        }
        return hits;
    }

    /// <summary>token 归一：小写、剔除非字母数字（与 VoiceSearchService
    /// .Normalize 同语义的单词级版）</summary>
    private static string NormalizeToken(string word)
    {
        var sb = new StringBuilder(word.Length);
        foreach (var c in word.ToLowerInvariant())
            if (char.IsLetterOrDigit(c)) sb.Append(c);
        return sb.ToString();
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
