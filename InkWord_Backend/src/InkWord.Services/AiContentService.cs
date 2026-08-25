using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using InkWord.Core.Entities;
using InkWord.Infrastructure.Cache;
using Microsoft.Extensions.AI;
using Microsoft.Extensions.Logging;

namespace InkWord.Services;

/// <summary>AI 内容类型：GradedExample 分级例句 / RootMnemonic 词根助记 / ConfusionNote 易混词辨析（仅审阅参考）/
/// DeckItems 卡组条目生成（v1.5 T5.4：素材→整副卡组条目，占位待审）</summary>
public enum AiContentKind
{
    GradedExample = 0,
    RootMnemonic = 1,
    ConfusionNote = 2,
    DeckItems = 3,
}

/// <summary>AI 生成建议（AiSuggestion 列的 JSON 载荷，camelCase）。
/// Front/Back/Phonetic/Meaning 为 T5.4 卡组条目建议字段（kind=3 专用，
/// 旧载荷缺字段反序列化为 null，向后兼容）</summary>
public record WordAiSuggestion(int Kind, string? Example = null, string? Root = null,
    string? ConfusionNote = null, string? Front = null, string? Back = null,
    string? Phonetic = null, string? Meaning = null);

/// <summary>
/// LLM 词库内容增强服务（M1 路径 B，2026-08-22）。
///
/// IChatClient 由 Program.cs 按 Ai:Provider 注册（本地 Ollama / OpenAI 兼容
/// 云 API 可切换），本服务只面向抽象。生成结果不直接落词库字段：暂存
/// Word.AiSuggestion 待人工审核（AdminWordController ai-apply / ai-reject）。
///
/// 长度红线：字节上限对齐固件 word_parser.h 缓冲（WORD_EXAMPLE_MAX 256 /
/// WORD_ROOT_MAX 96，含 NUL 与余量）——prompt 约束 + 代码字节级校验双保险，
/// 超限在字符边界安全截断，防止设备端解析溢出/丢字。
/// </summary>
public class AiContentService
{
    /// <summary>例句字节上限（固件 WORD_EXAMPLE_MAX=256B 减 NUL 与余量）</summary>
    public const int ExampleMaxBytes = 250;

    /// <summary>词根助记字节上限（固件 WORD_ROOT_MAX=96B 减 NUL 与余量）</summary>
    public const int RootMaxBytes = 90;

    /// <summary>卡面正面字节上限（固件 WORD_TEXT_MAX=64B 减 NUL 与余量，T5.4）</summary>
    public const int FrontMaxBytes = 60;

    /// <summary>音标/拼音字节上限（固件 WORD_PHONETIC_MAX=64B 减 NUL 与余量，T5.4）</summary>
    public const int PhoneticMaxBytes = 60;

    /// <summary>卡面背面字节上限（固件 WORD_MEANING_MAX=256B 减 NUL 与余量，T5.4）</summary>
    public const int BackMaxBytes = 250;

    /// <summary>T5.4 卡组单次生成条数上限（单次 LLM 调用输出 token 护栏）</summary>
    public const int DeckGenMaxItems = 40;

    /// <summary>T5.4 素材长度上限（字符；防 prompt 爆长）</summary>
    public const int SourceMaxChars = 20_000;

    private static readonly TimeSpan CacheTtl = TimeSpan.FromDays(7);

    /// <summary>默认科目（v1.3 T3.3 prompt 参数化；null/空 subject 回退此值，缓存键兼容不带后缀）</summary>
    public const string DefaultSubject = "英语";

    private readonly IChatClient _chat;
    private readonly IRedisCache _cache;
    private readonly ILogger<AiContentService> _logger;

    public AiContentService(IChatClient chat, IRedisCache cache, ILogger<AiContentService> logger)
    {
        _chat = chat;
        _cache = cache;
        _logger = logger;
    }

    /// <summary>
    /// 为词条生成一类 AI 建议；失败返回 null（调用方标记 AiStatus=3）。
    /// 结果写 Redis 缓存（wordId:kind[:subject]，TTL 7 天），驳回时由端点清除。
    /// subject：科目占位符（v1.3 T3.3），null/空 = 默认英语（缓存键不带后缀，与历史键兼容）。
    /// </summary>
    public async Task<WordAiSuggestion?> GenerateAsync(Word word, AiContentKind kind,
        string? subject, CancellationToken ct)
    {
        subject = NormalizeSubject(subject);
        var cacheKey = $"ai:sug:{word.Id}:{(int)kind}"
            + (subject == DefaultSubject ? "" : $":{subject}");
        var cached = await _cache.GetAsync<WordAiSuggestion>(cacheKey, ct);
        if (cached is not null) return cached;

        var (system, user) = BuildPrompt(word, kind, subject);
        try
        {
            var response = await _chat.GetResponseAsync(
            [
                new ChatMessage(ChatRole.System, system),
                new ChatMessage(ChatRole.User, user),
            ], new ChatOptions { Temperature = 0.7f }, ct);

            var suggestion = Parse(response.Text, kind);
            if (suggestion is null)
            {
                _logger.LogWarning("AI 建议解析失败：word={Word} kind={Kind} raw={Raw}",
                    word.Text, kind, TruncateForLog(response.Text));
                return null;
            }

            await _cache.SetAsync(cacheKey, suggestion, CacheTtl, ct);
            return suggestion;
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex, "AI 生成异常：word={Word} kind={Kind}", word.Text, kind);
            return null;
        }
    }

    /// <summary>
    /// T5.4 卡组条目批量生成：素材（课文/知识点清单）按目标卡组版式 prompt
    /// 模板生成整副条目建议；失败/空返回 null（调用方记日志，无行可标）。
    /// 结果写 Redis 缓存（deckId:素材哈希:limit，TTL 7 天）——同素材重触发
    /// 不重复消耗 token；驳回后重生成由 TTL 自然过期（素材级无单条复位）。
    /// 长度红线同既有建议：字节上限对齐固件 word_parser.h 缓冲，
    /// 超限在字符边界安全截断。
    /// </summary>
    public async Task<List<WordAiSuggestion>?> GenerateDeckItemsAsync(
        Deck deck, string? subject, string source, int limit, CancellationToken ct)
    {
        subject = NormalizeSubject(subject);
        limit = Math.Clamp(limit, 1, DeckGenMaxItems);
        source = source.Trim();
        if (source.Length > SourceMaxChars) source = source[..SourceMaxChars];

        var hash = Convert.ToHexString(
            SHA256.HashData(Encoding.UTF8.GetBytes(source)))[..12];
        var cacheKey = $"ai:deck:{deck.Id}:{hash}:{limit}";
        var cached = await _cache.GetAsync<List<WordAiSuggestion>>(cacheKey, ct);
        if (cached is not null) return cached;

        var (system, user) = BuildDeckPrompt(deck, subject, source, limit);
        try
        {
            var response = await _chat.GetResponseAsync(
            [
                new ChatMessage(ChatRole.System, system),
                new ChatMessage(ChatRole.User, user),
            ], new ChatOptions { Temperature = 0.4f }, ct);

            var items = ParseDeckItems(response.Text, deck.PayloadType);
            if (items.Count == 0)
            {
                _logger.LogWarning("AI 卡组生成解析失败：deck={Deck} raw={Raw}",
                    deck.Code, TruncateForLog(response.Text));
                return null;
            }
            if (items.Count > limit) items = items[..limit];

            await _cache.SetAsync(cacheKey, items, CacheTtl, ct);
            return items;
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex, "AI 卡组生成异常：deck={Deck}", deck.Code);
            return null;
        }
    }

    /// <summary>驳回建议时清除缓存（M2：驳回后不缓存，下次重新生成）。
    /// 仅清默认科目键；非默认科目缓存由 TTL 7 天自然过期（v1.5 全科前无驳回跨科目场景）。</summary>
    public Task InvalidateCacheAsync(Guid wordId, AiContentKind kind, CancellationToken ct) =>
        _cache.RemoveAsync($"ai:sug:{wordId}:{(int)kind}", ct);

    /// <summary>科目归一：null/空白回退默认英语，去除首尾空白（进缓存键与 prompt）</summary>
    internal static string NormalizeSubject(string? subject) =>
        string.IsNullOrWhiteSpace(subject) ? DefaultSubject : subject.Trim();

    // ---- Prompt ----

    /// <summary>prompt 模板（v1.3 T3.3 科目占位符化：{subject} 注入教师/例句身份，
    /// 其余约束（长度红线/JSON 格式）不变；v1.5 全科生成的前提）</summary>
    private static (string System, string User) BuildPrompt(Word word, AiContentKind kind, string subject) => kind switch
    {
        AiContentKind.GradedExample => (
            $"你是资深的初中{subject}教师。为给定单词生成一条适合学生年级水平的例句。"
            + "严格输出 JSON：{\"example\":\"英文例句\",\"translation\":\"中文翻译\"}，不要输出其他内容。"
            + $"要求：{subject}例句不超过 110 个字符且必须包含单词原形；总输出（{subject}原文+中文）UTF-8 字节数不超过 {ExampleMaxBytes}；"
            + "句式贴合学生认知，用词不超纲。",
            $"单词：{word.Text}\n释义：{word.Meaning}\n年级：{(string.IsNullOrEmpty(word.Grade) ? "九年级" : word.Grade)}\n难度：{word.Difficulty}/5"),

        AiContentKind.RootMnemonic => (
            $"你是{subject}词源专家。为给定单词编写一条基于词根词缀的趣味助记。"
            + "严格输出 JSON：{\"root\":\"助记文本\"}，不要输出其他内容。"
            + $"要求：格式类似 \"spect=看 → inspect 检查(向内看)\"，以 ASCII 词根开头、中文点睛；"
            + $"UTF-8 字节数不超过 {RootMaxBytes}（中文每字约 3 字节，控制在 25 个汉字内）。",
            $"单词：{word.Text}\n释义：{word.Meaning}\n已有词根：{(string.IsNullOrEmpty(word.Root) ? "（无）" : word.Root)}"),

        _ => (
            $"你是{subject}词汇专家。指出与给定单词最易混淆的 1~2 个词并给出简明辨析。"
            + "严格输出 JSON：{\"confusionNote\":\"辨析文本\"}，不要输出其他内容。"
            + "要求：形近/义近各最多 1 个，格式如 \"adapt 适应 vs adopt 采纳：a-dapt'装'，a-adopt'选'\"；"
            + "不超过 120 个汉字。此内容仅供人工审阅参考，不下发设备。",
            $"单词：{word.Text}\n释义：{word.Meaning}"),
    };

    /// <summary>T5.4 卡组生成 prompt（科目×版式双维模板）：课文→默写卡
    /// （上句/下句/拼音/译文）；知识点→问答卡；单词清单→单词卡。
    /// 约束对齐设备缓冲（AiContentService 常量同源）与 words.json 契约</summary>
    private static (string System, string User) BuildDeckPrompt(
        Deck deck, string subject, string source, int limit) => deck.PayloadType switch
    {
        "poem-card" => (
            $"你是资深的{subject}教师，精通古诗文默写教学。从给定素材中挑选适合默写训练的名句，生成上句→下句配对卡。"
            + "严格输出 JSON 数组：[{\"front\":\"上句\",\"back\":\"下句\",\"phonetic\":\"下句拼音\",\"meaning\":\"译文\"}]，不要输出其他内容。"
            + "要求：上下句必须原文连续对应；每句不超过 12 个汉字（不含标点）；"
            + "phonetic 为下句带声调拼音（字母+声调符号，空格分隔）；meaning 为整句白话译文，不超过 40 个汉字；"
            + $"只选名篇名句，最多 {limit} 条。",
            $"科目：{subject}\n卡组：{deck.Name}\n素材：\n{source}"),

        "qa-card" => (
            $"你是资深的{subject}教师。把给定知识点素材转成问答卡（题面→答案）。"
            + "严格输出 JSON 数组：[{\"front\":\"题面\",\"back\":\"答案\"}]，不要输出其他内容。"
            + "要求：题面是明确的问题或填空，不超过 30 个汉字；答案精确简明，不超过 50 个汉字；"
            + $"覆盖素材核心知识点，最多 {limit} 条。",
            $"科目：{subject}\n卡组：{deck.Name}\n素材：\n{source}"),

        _ => (
            $"你是资深的{subject}教师。为给定单词/词条清单生成单词卡。"
            + "严格输出 JSON 数组：[{\"front\":\"单词\",\"back\":\"中文释义\",\"phonetic\":\"音标\",\"example\":\"例句（英文（中文翻译）单行）\"}]，不要输出其他内容。"
            + "要求：front 用素材原词；释义不超过 20 个汉字；例句包含该词，格式「英文句子（中文翻译）」，总长不超过 80 个字符；"
            + $"最多 {limit} 条。",
            $"科目：{subject}\n卡组：{deck.Name}\n素材：\n{source}"),
    };

    // ---- 解析与长度防护 ----

    internal static WordAiSuggestion? Parse(string raw, AiContentKind kind)
    {
        var json = ExtractJsonObject(raw);
        if (json is null) return null;

        try
        {
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;

            string? example = null, root2 = null, note = null;
            if (root.TryGetProperty("example", out var ex) && ex.ValueKind == JsonValueKind.String)
            {
                // 例句合并英文与中文翻译为一行（设备 Example 单行渲染）
                var en = ex.GetString() ?? "";
                var zh = root.TryGetProperty("translation", out var tr) && tr.ValueKind == JsonValueKind.String
                    ? tr.GetString() : null;
                example = string.IsNullOrEmpty(zh) ? en : $"{en}（{zh}）";
                example = TruncateUtf8(example.Trim(), ExampleMaxBytes);
                if (example.Length == 0) example = null;
            }
            if (root.TryGetProperty("root", out var rt) && rt.ValueKind == JsonValueKind.String)
            {
                root2 = TruncateUtf8((rt.GetString() ?? "").Trim(), RootMaxBytes);
                if (root2.Length == 0) root2 = null;
            }
            if (root.TryGetProperty("confusionNote", out var cn) && cn.ValueKind == JsonValueKind.String)
            {
                note = TruncateUtf8((cn.GetString() ?? "").Trim(), 360);
                if (note.Length == 0) note = null;
            }

            var result = new WordAiSuggestion((int)kind, example, root2, note);
            return HasContent(result, kind) ? result : null;
        }
        catch (JsonException)
        {
            return null;
        }
    }

    private static bool HasContent(WordAiSuggestion s, AiContentKind kind) => kind switch
    {
        AiContentKind.GradedExample => s.Example is not null,
        AiContentKind.RootMnemonic => s.Root is not null,
        _ => s.ConfusionNote is not null,
    };

    /// <summary>T5.4 解析卡组条目建议（LLM 输出 JSON 数组→版式归一+截断+去重）。
    /// 环绕 markdown 栅栏/非数组/坏 JSON 一律返回空列表（调用方记日志）</summary>
    public static List<WordAiSuggestion> ParseDeckItems(string raw, string? payloadType)
    {
        payloadType = string.IsNullOrEmpty(payloadType) ? "word-card" : payloadType;
        var json = ExtractJsonArray(raw);
        if (json is null) return [];

        try
        {
            using var doc = JsonDocument.Parse(json);
            if (doc.RootElement.ValueKind != JsonValueKind.Array) return [];

            var seen = new HashSet<string>(StringComparer.Ordinal);
            var list = new List<WordAiSuggestion>();
            foreach (var el in doc.RootElement.EnumerateArray())
            {
                if (el.ValueKind != JsonValueKind.Object) continue;
                var front = TruncateUtf8(Str(el, "front") ?? "", FrontMaxBytes);
                var back = TruncateUtf8(Str(el, "back") ?? "", BackMaxBytes);
                if (front.Length == 0 || back.Length == 0) continue;
                if (!seen.Add(front)) continue;

                if (payloadType == "poem-card")
                {
                    list.Add(new WordAiSuggestion((int)AiContentKind.DeckItems,
                        Front: front, Back: back,
                        Phonetic: NullIfEmpty(TruncateUtf8(Str(el, "phonetic") ?? "", PhoneticMaxBytes)),
                        Meaning: NullIfEmpty(TruncateUtf8(Str(el, "meaning") ?? "", BackMaxBytes))));
                }
                else if (payloadType == "qa-card")
                {
                    list.Add(new WordAiSuggestion((int)AiContentKind.DeckItems,
                        Front: front, Back: back));
                }
                else // word-card（缺省/未知回退，与固件 card_layout 同口径）
                {
                    list.Add(new WordAiSuggestion((int)AiContentKind.DeckItems,
                        Front: front, Back: back,
                        Phonetic: NullIfEmpty(TruncateUtf8(Str(el, "phonetic") ?? "", PhoneticMaxBytes)),
                        Example: NullIfEmpty(TruncateUtf8(Str(el, "example") ?? "", ExampleMaxBytes))));
                }
            }
            return list;
        }
        catch (JsonException)
        {
            return [];
        }
    }

    /// <summary>T5.4 审校通过时按目标卡组版式写入 Word 正字段（占位行→正式条目）：
    /// poem 对齐 T4.4 云通道契约（text=下句/root=上句/phonetic=下句拼音/
    /// meaning=译文 + PayloadJson={"prev":上句}）；word/qa 对齐 T4.3
    /// （text=题面/单词、meaning=答案/释义）。reqXxx 为人工编辑终值（优先）。</summary>
    public static void ApplyDeckSuggestion(Word word, string? payloadType,
        WordAiSuggestion sug, string? reqFront, string? reqBack,
        string? reqPhonetic, string? reqMeaning, string? reqExample)
    {
        payloadType = string.IsNullOrEmpty(payloadType) ? "word-card" : payloadType;
        var front = TruncateUtf8((reqFront ?? sug.Front ?? word.Front).Trim(), FrontMaxBytes);
        var back = TruncateUtf8((reqBack ?? sug.Back ?? word.Back).Trim(), BackMaxBytes);
        word.Front = front;
        word.Back = back;
        word.Phonetic = TruncateUtf8((reqPhonetic ?? sug.Phonetic ?? "").Trim(), PhoneticMaxBytes);

        if (payloadType == "poem-card")
        {
            word.Text = TruncateUtf8(back, FrontMaxBytes);
            word.Root = TruncateUtf8(front, RootMaxBytes);
            word.Meaning = TruncateUtf8((reqMeaning ?? sug.Meaning ?? "").Trim(), BackMaxBytes);
            word.Example = "";
            word.PayloadJson = JsonSerializer.Serialize(new { prev = front });
        }
        else
        {
            word.Text = front;
            word.Meaning = back;
            word.Example = TruncateUtf8((reqExample ?? sug.Example ?? word.Example).Trim(), ExampleMaxBytes);
        }
    }

    private static string? Str(JsonElement el, string name) =>
        el.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String
            ? v.GetString() : null;

    private static string? NullIfEmpty(string s) => s.Length == 0 ? null : s;

    /// <summary>剥离 markdown 代码栅栏，提取首个完整 JSON 对象文本</summary>
    internal static string? ExtractJsonObject(string raw)
    {
        if (string.IsNullOrWhiteSpace(raw)) return null;
        var start = raw.IndexOf('{');
        if (start < 0) return null;
        var end = raw.LastIndexOf('}');
        return end > start ? raw[start..(end + 1)] : null;
    }

    /// <summary>提取首个完整 JSON 数组文本（T5.4 卡组批量输出形态）</summary>
    public static string? ExtractJsonArray(string raw)
    {
        if (string.IsNullOrWhiteSpace(raw)) return null;
        var start = raw.IndexOf('[');
        if (start < 0) return null;
        var end = raw.LastIndexOf(']');
        return end > start ? raw[start..(end + 1)] : null;
    }

    /// <summary>按 UTF-8 字节数截断（在完整字符边界回退，不产生乱码尾字节）</summary>
    internal static string TruncateUtf8(string s, int maxBytes)
    {
        if (Encoding.UTF8.GetByteCount(s) <= maxBytes) return s;
        var bytes = Encoding.UTF8.GetBytes(s);
        var cut = maxBytes;
        // 回退到非续字节位置（UTF-8 续字节 10xxxxxx = 0x80~0xBF）
        while (cut > 0 && (bytes[cut] & 0xC0) == 0x80) cut--;
        return Encoding.UTF8.GetString(bytes, 0, cut).TrimEnd();
    }

    private static string TruncateForLog(string s) => s.Length <= 200 ? s : s[..200] + "...";
}
