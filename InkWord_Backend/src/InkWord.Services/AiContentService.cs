using System.Text;
using System.Text.Json;
using InkWord.Core.Entities;
using InkWord.Infrastructure.Cache;
using Microsoft.Extensions.AI;
using Microsoft.Extensions.Logging;

namespace InkWord.Services;

/// <summary>AI 内容类型：GradedExample 分级例句 / RootMnemonic 词根助记 / ConfusionNote 易混词辨析（仅审阅参考）</summary>
public enum AiContentKind
{
    GradedExample = 0,
    RootMnemonic = 1,
    ConfusionNote = 2,
}

/// <summary>AI 生成建议（AiSuggestion 列的 JSON 载荷，camelCase）</summary>
public record WordAiSuggestion(int Kind, string? Example, string? Root, string? ConfusionNote);

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

    private static readonly TimeSpan CacheTtl = TimeSpan.FromDays(7);

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
    /// 结果写 Redis 缓存（wordId:kind，TTL 7 天），驳回时由端点清除。
    /// </summary>
    public async Task<WordAiSuggestion?> GenerateAsync(Word word, AiContentKind kind, CancellationToken ct)
    {
        var cacheKey = $"ai:sug:{word.Id}:{(int)kind}";
        var cached = await _cache.GetAsync<WordAiSuggestion>(cacheKey, ct);
        if (cached is not null) return cached;

        var (system, user) = BuildPrompt(word, kind);
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

    /// <summary>驳回建议时清除缓存（M2：驳回后不缓存，下次重新生成）</summary>
    public Task InvalidateCacheAsync(Guid wordId, AiContentKind kind, CancellationToken ct) =>
        _cache.RemoveAsync($"ai:sug:{wordId}:{(int)kind}", ct);

    // ---- Prompt ----

    private static (string System, string User) BuildPrompt(Word word, AiContentKind kind) => kind switch
    {
        AiContentKind.GradedExample => (
            "你是资深的初中英语教师。为给定单词生成一条适合学生年级水平的例句。"
            + "严格输出 JSON：{\"example\":\"英文例句\",\"translation\":\"中文翻译\"}，不要输出其他内容。"
            + $"要求：英文例句不超过 110 个字符且必须包含单词原形；总输出（英文+中文）UTF-8 字节数不超过 {ExampleMaxBytes}；"
            + "句式贴合学生认知，用词不超纲。",
            $"单词：{word.Text}\n释义：{word.Meaning}\n年级：{(string.IsNullOrEmpty(word.Grade) ? "九年级" : word.Grade)}\n难度：{word.Difficulty}/5"),

        AiContentKind.RootMnemonic => (
            "你是词源专家。为给定单词编写一条基于词根词缀的趣味助记。"
            + "严格输出 JSON：{\"root\":\"助记文本\"}，不要输出其他内容。"
            + $"要求：格式类似 \"spect=看 → inspect 检查(向内看)\"，以 ASCII 词根开头、中文点睛；"
            + $"UTF-8 字节数不超过 {RootMaxBytes}（中文每字约 3 字节，控制在 25 个汉字内）。",
            $"单词：{word.Text}\n释义：{word.Meaning}\n已有词根：{(string.IsNullOrEmpty(word.Root) ? "（无）" : word.Root)}"),

        _ => (
            "你是英语词汇专家。指出与给定单词最易混淆的 1~2 个词并给出简明辨析。"
            + "严格输出 JSON：{\"confusionNote\":\"辨析文本\"}，不要输出其他内容。"
            + "要求：形近/义近各最多 1 个，格式如 \"adapt 适应 vs adopt 采纳：a-dapt'装'，a-adopt'选'\"；"
            + "不超过 120 个汉字。此内容仅供人工审阅参考，不下发设备。",
            $"单词：{word.Text}\n释义：{word.Meaning}"),
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

    /// <summary>剥离 markdown 代码栅栏，提取首个完整 JSON 对象文本</summary>
    internal static string? ExtractJsonObject(string raw)
    {
        if (string.IsNullOrWhiteSpace(raw)) return null;
        var start = raw.IndexOf('{');
        if (start < 0) return null;
        var end = raw.LastIndexOf('}');
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
