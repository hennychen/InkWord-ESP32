using System.Text;
using System.Runtime.CompilerServices;
using System.Text.Json;
using InkWord.Core.Entities;
using InkWord.Infrastructure.Cache;
using InkWord.Services;
using Microsoft.Extensions.AI;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.Logging.Abstractions;
using Xunit;

namespace InkWord.Tests;

/// <summary>
/// ChatService 编排逻辑测试（P2A，2026-08-24）。
///
/// ASR/LLM/Redis 三依赖手写替身（IChatClient 依 Microsoft.Extensions.AI
/// 抽象）；TtsService 用真实例 + 假合成引擎（TtsServiceTests 同款）。
/// 重点覆盖：降级路径（Redis 挂/ASR 挂/LLM 挂/超限 wav）、长度截断、
/// 黑名单替换、上下文 12 轮裁剪与 TTL 回写。
/// </summary>
public class ChatServiceTests : IDisposable
{
    private readonly string _dir;
    private readonly TtsService _tts;
    private readonly FakeSynthesizer _engine = new();
    private readonly FakeAsr _asr = new();
    private readonly FakeChat _chat = new();
    private readonly FakeRedis _redis = new();
    private readonly ChatService _svc;

    public ChatServiceTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), $"inkword-chat-test-{Guid.NewGuid():N}");
        var cfg = new ConfigurationBuilder()
            .AddInMemoryCollection(new Dictionary<string, string?>
            {
                ["Tts:AudioDir"] = _dir,
                ["Ai:Model"] = "qwen2.5:7b",
            })
            .Build();
        _tts = new TtsService(_engine, cfg, new NullLogger<TtsService>());
        _svc = new ChatService(_asr, _chat, _tts, _redis, cfg, new NullLogger<ChatService>());
    }

    public void Dispose()
    {
        if (Directory.Exists(_dir)) Directory.Delete(_dir, recursive: true);
    }

    // ---- 编排主链路 ----

    [Fact]
    public async Task Converse_HappyPath_ReturnsReplyAudioAndContext()
    {
        _asr.NextText = "hello teacher";
        _chat.NextReply = "Hi! What did you learn today?";   // 恰 2 句，护栏不截断
        _engine.NextResult = new byte[] { 1, 2, 3 };

        var device = Guid.NewGuid();
        var reply = await _svc.ConverseAsync(device, Wav(1600), CancellationToken.None);

        Assert.Equal("hello teacher", reply.Transcript);
        Assert.Equal(_chat.NextReply, reply.Reply);
        Assert.Equal("qwen2.5:7b", reply.Engine);
        Assert.Equal("en", reply.Lang);   // A2 默认英语路由
        Assert.NotNull(reply.AudioUrl);
        Assert.StartsWith("/api/device/audio/chat_", reply.AudioUrl);
        Assert.True(File.Exists(Path.Combine(_dir, Path.GetFileName(reply.AudioUrl!))));
        // 上下文回写：本轮 user+assistant 共 2 条
        var stored = _redis.ReadStore<List<ChatMsg>>($"chat:{device:N}");
        Assert.Equal(2, stored!.Count);
        Assert.Equal("user", stored[0].Role);
    }

    [Fact]
    public async Task Converse_TtsFailure_AudioUrlNullReplyIntact()
    {
        _asr.NextText = "hi";
        _chat.NextReply = "Hello!";
        _engine.NextResult = null;   // 合成失败

        var reply = await _svc.ConverseAsync(Guid.NewGuid(), Wav(1600), CancellationToken.None);

        Assert.Null(reply.AudioUrl);
        Assert.Equal("Hello!", reply.Reply);
    }

    // ---- 降级与错误路径 ----

    [Theory]
    [InlineData(0)]    // 引擎不可用
    [InlineData(1)]    // 空转写（无话音）
    public async Task Converse_AsrFailure_Throws(int mode)
    {
        _asr.Mode = mode;
        _chat.NextReply = "x";
        var ex = await Assert.ThrowsAsync<ChatException>(
            () => _svc.ConverseAsync(Guid.NewGuid(), Wav(1600), CancellationToken.None));
        Assert.Equal(mode == 0 ? 503 : 400, ex.StatusCode);
    }

    [Fact]
    public async Task Converse_LlmFailure_Throws502()
    {
        _asr.NextText = "hi";
        _chat.NextError = new InvalidOperationException("ollama down");
        var ex = await Assert.ThrowsAsync<ChatException>(
            () => _svc.ConverseAsync(Guid.NewGuid(), Wav(1600), CancellationToken.None));
        Assert.Equal(502, ex.StatusCode);
    }

    [Fact]
    public async Task Converse_InvalidWav_Throws400()
    {
        _asr.NextText = "hi";
        var ex = await Assert.ThrowsAsync<ChatException>(
            () => _svc.ConverseAsync(Guid.NewGuid(), "not a wav"u8.ToArray(), CancellationToken.None));
        Assert.Equal(400, ex.StatusCode);
    }

    [Fact]
    public async Task Converse_OversizedWav_Throws400()
    {
        _asr.NextText = "hi";
        // 合法 RIFF 头 + 512KB 以上 data（262144 样本 × 2B + 44B 头）
        var ex = await Assert.ThrowsAsync<ChatException>(
            () => _svc.ConverseAsync(Guid.NewGuid(), Wav(262_144), CancellationToken.None));
        Assert.Equal(400, ex.StatusCode);
        Assert.Contains("large", ex.Message);
    }

    [Fact]
    public async Task Converse_RedisDown_StillConverses()
    {
        _asr.NextText = "hello";
        _chat.NextReply = "Hi there!";
        _redis.Fail = true;

        var reply = await _svc.ConverseAsync(Guid.NewGuid(), Wav(1600), CancellationToken.None);

        Assert.Equal("Hi there!", reply.Reply);   // Redis 挂不影响本轮
    }

    // ---- 安全护栏 ----

    [Fact]
    public async Task Converse_BlocklistHit_ReplacedWithSafeReply()
    {
        _asr.NextText = "tell me a story";
        _chat.NextReply = "Once there was a pirate with a gun and a map.";
        _engine.NextResult = new byte[] { 1 };

        var reply = await _svc.ConverseAsync(Guid.NewGuid(), Wav(1600), CancellationToken.None);

        Assert.DoesNotContain("gun", reply.Reply, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("favorite animal", reply.Reply);   // 安全引导语
    }

    [Theory]
    [InlineData("a. b. c. d.", "a. b.")]                                        // 句截断 ≤2
    [InlineData("one two three four five", "one two three four five")]           // 不超不动
    public void EnforceLimits_TruncatesSentences(string input, string expected)
    {
        Assert.Equal(expected, ChatService.EnforceLimits(input));
    }

    [Fact]
    public void EnforceLimits_TruncatesWords()
    {
        var many = string.Join(' ', Enumerable.Range(1, 50).Select(i => $"w{i}"));
        var result = ChatService.EnforceLimits(many);
        Assert.True(result.Split(' ').Length <= 40);
    }

    // ---- A1 模式参数化（2026-08-28）----

    [Fact]
    public async Task Converse_ScenarioMode_InjectsPromptWarmupAndIsolatedContext()
    {
        _asr.NextText = "i want a hamburger";
        _chat.NextReply = "Great choice! Would you like fries too?";
        _engine.NextResult = new byte[] { 1 };
        var device = Guid.NewGuid();

        var reply = await _svc.ConverseAsync(device, Wav(1600),
            ChatService.ModeScenario, "food", CancellationToken.None);

        // prompt 注入：persona + 场景设定 + 目标词
        var sys = _chat.LastMessages![0].Text;
        Assert.Contains("waiter", sys);
        Assert.Contains("ordering lunch", sys);
        Assert.Contains("order", sys);
        // 首轮：warmup 中文预热 + 开场白 assistant 前缀入上下文
        Assert.Equal(ChatService.ModeScenario, reply.Mode);
        Assert.NotNull(reply.Warmup);
        Assert.Contains("order", reply.Warmup);
        // 上下文键隔离：scenario 落 :s:food，free 键不受影响
        Assert.Null(_redis.ReadStore<List<ChatMsg>>($"chat:{device:N}"));
        var stored = _redis.ReadStore<List<ChatMsg>>($"chat:{device:N}:s:food");
        Assert.Equal(3, stored!.Count);   // 开场白 + 本轮 user + assistant
        Assert.Equal("assistant", stored[0].Role);
        Assert.Contains("menu", stored[0].Text);
    }

    [Fact]
    public async Task Converse_ScenarioSecondRound_WarmupNull()
    {
        var device = Guid.NewGuid();
        _asr.NextText = "yes please";
        _chat.NextReply = "Here you are!";
        _engine.NextResult = new byte[] { 1 };
        _redis.WriteStore($"chat:{device:N}:s:food",
            new List<ChatMsg> { new("assistant", "Welcome!") });

        var reply = await _svc.ConverseAsync(device, Wav(1600),
            ChatService.ModeScenario, "food", CancellationToken.None);

        Assert.Null(reply.Warmup);
    }

    [Fact]
    public async Task Converse_TranslateMode_TwoLineReplyTtsEnglishOnly()
    {
        _asr.NextText = "i go to school yesterday";
        _chat.NextReply = "I went to school yesterday.\n我昨天去了学校。";
        _engine.NextResult = new byte[] { 1 };
        var device = Guid.NewGuid();

        var reply = await _svc.ConverseAsync(device, Wav(1600),
            ChatService.ModeTranslate, null, CancellationToken.None);

        Assert.Equal(ChatService.ModeTranslate, reply.Mode);
        Assert.Null(reply.Warmup);
        Assert.Contains("我昨天", reply.Reply);
        Assert.NotNull(reply.AudioUrl);
        Assert.Equal("I went to school yesterday.", _engine.LastText);   // TTS 只收英文行
        Assert.Equal("en", _engine.LastLang);
        Assert.Equal("en", reply.Lang);
        Assert.Equal(0.4f, _chat.LastOptions!.Temperature);              // 翻译准确性优先
        Assert.NotNull(_redis.ReadStore<List<ChatMsg>>($"chat:{device:N}:t"));
    }

    [Fact]
    public async Task Converse_TranslateMalformedAllChinese_SkipsTts()
    {
        _asr.NextText = "hello";
        _chat.NextReply = "你好。\n最近怎么样？";   // LLM 未按两行格式（全中文）

        var reply = await _svc.ConverseAsync(Guid.NewGuid(), Wav(1600),
            ChatService.ModeTranslate, null, CancellationToken.None);

        Assert.Null(reply.AudioUrl);
        Assert.Equal(0, _engine.Calls);   // 无纯英文行可送，跳过合成；
                                          // 部分英文行则只送该行（上例覆盖）
    }

    [Fact]
    public async Task Converse_TranslateLongReply_TruncatedToByteBudget()
    {
        _asr.NextText = "hi";
        _chat.NextReply = $"A very long english sentence here.\n{new string('学', 200)}";

        var reply = await _svc.ConverseAsync(Guid.NewGuid(), Wav(1600),
            ChatService.ModeTranslate, null, CancellationToken.None);

        // 固件 CHAT_REPLY_MAX=256B 红线（UTF-8 字节口径，rune 边界安全）
        Assert.True(Encoding.UTF8.GetByteCount(reply.Reply) <= ChatService.TranslateMaxBytes);
    }

    [Theory]
    [InlineData("hacker", null)]     // 非法 mode
    [InlineData("scenario", null)]   // scenario 缺 scenarioId
    [InlineData("scenario", "nope")] // 未知场景
    public async Task Converse_InvalidModeOrScenario_Throws400(string mode, string? sid)
    {
        _asr.NextText = "hi";
        var ex = await Assert.ThrowsAsync<ChatException>(
            () => _svc.ConverseAsync(Guid.NewGuid(), Wav(1600), mode, sid, CancellationToken.None));
        Assert.Equal(400, ex.StatusCode);
    }

    [Fact]
    public async Task Converse_LegacyWrapper_EquivalentToFreeMode()
    {
        _asr.NextText = "hi";
        _chat.NextReply = "Hello there!";
        _engine.NextResult = new byte[] { 1 };
        var device = Guid.NewGuid();

        var reply = await _svc.ConverseAsync(device, Wav(1600), CancellationToken.None);

        Assert.Equal(ChatService.ModeFree, reply.Mode);
        // free 键保持现状（老会话无缝续用，现有用例同键断言即等价性锺点）
        var stored = _redis.ReadStore<List<ChatMsg>>($"chat:{device:N}");
        Assert.Equal(2, stored!.Count);
    }

    [Fact]
    public void TruncateUtf8_CutsOnRuneBoundary()
    {
        var zh = new string('学', 100);   // 300 UTF-8 字节
        var cut = ChatService.TruncateUtf8(zh, 240);
        Assert.Equal(80, cut.Length);   // 240/3=80 字，无半字
        Assert.Equal(240, Encoding.UTF8.GetByteCount(cut));
        Assert.Equal("abc", ChatService.TruncateUtf8("abc", 10));   // ASCII 不动
    }

    [Theory]
    [InlineData("Good day.\n好日子。", "Good day.")]
    [InlineData("好日子。\nGood day.", "Good day.")]   // 跳过中文行
    [InlineData("全是中文。\n另一行。", null)]             // 无英文行
    public void FirstNonChineseLine_PicksEnglishLine(string input, string? expected)
    {
        Assert.Equal(expected, ChatService.FirstNonChineseLine(input));
    }

    [Fact]
    public async Task Converse_FreeModeChineseReply_TtsRoutesZhVoice()
    {
        _asr.NextText = "ni hao";
        _chat.NextReply = "你好！今天想聊什么？";   // LLM 适应中文提问回中文
        _engine.NextResult = new byte[] { 1 };

        var reply = await _svc.ConverseAsync(Guid.NewGuid(), Wav(1600), CancellationToken.None);

        // A2 free 模式语言路由：中文回复 zh 音色播报全文（mode 不变）
        Assert.Equal("zh", reply.Lang);
        Assert.Equal("zh", _engine.LastLang);
        Assert.Equal("你好！今天想聊什么？", _engine.LastText);
        Assert.NotNull(reply.AudioUrl);
    }

    // ---- A3：异步落库与生词命中 ----

    [Fact]
    public async Task Converse_TurnSinkEnqueuesScenarioTurnWithModeFields()
    {
        _asr.NextText = "a table for two";
        _chat.NextReply = "Sure! Right this way.";
        _engine.NextResult = new byte[] { 1 };
        var cfg = new ConfigurationBuilder().AddInMemoryCollection(
            new Dictionary<string, string?> { ["Tts:AudioDir"] = _dir, ["Ai:Model"] = "qwen2.5:7b" }).Build();
        var sink = new FakeTurnSink();
        var svc = new ChatService(_asr, _chat, _tts, _redis, cfg,
            new NullLogger<ChatService>(), sink);
        var device = Guid.NewGuid();

        await svc.ConverseAsync(device, Wav(1600), "scenario", "food", CancellationToken.None);

        var turn = Assert.Single(sink.Turns);
        Assert.Equal(device, turn.DeviceId);
        Assert.Equal("scenario", turn.Mode);
        Assert.Equal("food", turn.ScenarioId);
        Assert.Equal("a table for two", turn.Transcript);
        Assert.Equal(_chat.NextReply, turn.Reply);
    }

    [Fact]
    public async Task Converse_NoOptionalDeps_LegacyPathUntouched()
    {
        // 缺省不带 sink/vocab：wordHits 为 null 且不抛——A3 对存量用例零侵入的锺点
        _asr.NextText = "hi";
        _chat.NextReply = "Hello there!";
        var reply = await _svc.ConverseAsync(Guid.NewGuid(), Wav(1600), CancellationToken.None);
        Assert.Null(reply.WordHits);
    }

    [Fact]
    public async Task Converse_WordHits_CaseInsensitiveDedupCap5()
    {
        _asr.NextText = "I like Apple and apple pie";   // Apple 大小写归一去重 1 条
        _chat.NextReply = "Do you also like banana, cherry, date, elder, fig, grape?";
        _engine.NextResult = new byte[] { 1 };
        var vocab = new FakeVocab(new[]
        {
            new VocabEntry("apple", "w1"), new VocabEntry("banana", "w2"),
            new VocabEntry("cherry", "w3"), new VocabEntry("date", "w4"),
            new VocabEntry("elder", "w5"), new VocabEntry("fig", "w6"),
            new VocabEntry("grape", "w7"),
        });
        var cfg = new ConfigurationBuilder().AddInMemoryCollection(
            new Dictionary<string, string?> { ["Tts:AudioDir"] = _dir, ["Ai:Model"] = "qwen2.5:7b" }).Build();
        var svc = new ChatService(_asr, _chat, _tts, _redis, cfg,
            new NullLogger<ChatService>(), null, vocab);

        var reply = await svc.ConverseAsync(Guid.NewGuid(), Wav(1600), CancellationToken.None);

        // 首现序取前 5（apple→elder），fig/grape 溢出丢弃；标点随 token 剥离
        Assert.Equal(new[] { "apple", "banana", "cherry", "date", "elder" },
            reply.WordHits!.Select(h => h.Text).ToArray());
        Assert.All(reply.WordHits!, h => Assert.StartsWith("w", h.CloudId));
    }

    [Theory]
    [InlineData("Hello, WORLD!", "world", true)]   // 标点剥离 + 大小写归一
    [InlineData("playground", "ground", false)]    // 整 token 精确命中，不做子串
    [InlineData("", "word", false)]                // 空文本
    public void MatchWordHits_NormalizesPunctuationAndWholeTokenOnly(
        string text, string word, bool hit)
    {
        var vocab = new[] { new VocabEntry(word, "w1") };
        var hits = ChatService.MatchWordHits(text, vocab);
        Assert.Equal(hit ? 1 : 0, hits.Count);
    }

    // ---- emoji 剥离（屏显红线，2026-08-29 三云实测发现） ----

    [Theory]
    [InlineData("Yes, it's a beautiful day! \uD83C\uDF1E Do you like it?",
        "Yes, it's a beautiful day! Do you like it?")]        // emoji 剥离且空格收敛
    [InlineData("今天天气\u2600\uFE0F很好，继续加油！",
        "今天天气很好，继续加油！")]                             // 中文与中文标点原样保留
    [InlineData("No emoji here.", "No emoji here.")]           // 无 emoji 原文返回
    public void StripEmoji_RemovesEmojiAndKeepsCjk(string input, string expected)
    {
        Assert.Equal(expected, ChatService.StripEmoji(input));
    }

    // ---- 会话上下文 ----

    [Fact]
    public async Task Converse_SecondRound_CarriesHistoryAndCompacts()
    {
        var device = Guid.NewGuid();
        _asr.NextText = "what is this";
        _chat.NextReply = "It is a pen.";
        _engine.NextResult = new byte[] { 1 };

        await _svc.ConverseAsync(device, Wav(1600), CancellationToken.None);
        // 第二轮前预塞历史超上限：头部 12 条摘要压缩 + 近因保留（P1-2）
        var history = Enumerable.Range(0, 23)
            .Select(i => new ChatMsg(i % 2 == 0 ? "user" : "assistant", $"old {i}"))
            .ToList();
        _redis.WriteStore($"chat:{device:N}", history);

        await _svc.ConverseAsync(device, Wav(1600), CancellationToken.None);

        var stored = _redis.ReadStore<List<ChatMsg>>($"chat:{device:N}")!;
        Assert.Equal(14, stored.Count);   // 23 预置 + 2 本轮 = 25 → 头 12 条压 1：1 + 13
        Assert.Equal("assistant", stored[0].Role);
        Assert.StartsWith("[Earlier chat summary] ", stored[0].Text);
        Assert.Contains("pen", stored[0].Text);   // 摘要即 FakeChat 回复文本
        Assert.Equal("old 12", stored[1].Text);   // 近因上下文原样保留（第 13 条起）
        // 摘要调用收到含头部历史的压缩 prompt（LastMessages 被末次调用覆盖）
        var prompt = Assert.Single(_chat.LastMessages!);
        Assert.Equal(ChatRole.User, prompt.Role);
        Assert.Contains("old 0", prompt.Text);
        Assert.Contains("old 11", prompt.Text);          // 头 12 条全进摘要
        Assert.DoesNotContain("old 12", prompt.Text);    // 近因条目不进摘要
    }

    // ---- 流式管线（P0-1，2026-08-30）----

    /// <summary>跑一轮流式对话收集 NDJSON 行（emitLine 即“网络写出”）</summary>
    private async Task<List<string>> StreamRound(Guid device, string? mode = null,
        string? scenarioId = null)
    {
        var lines = new List<string>();
        await _svc.ConverseStreamAsync(device, Wav(1600), mode, scenarioId,
            l => { lines.Add(l); return Task.CompletedTask; }, CancellationToken.None);
        return lines;
    }

    private static JsonElement Obj(string line) => JsonDocument.Parse(line).RootElement;
    private static string Kind(JsonElement e) => e.GetProperty("t").GetString()!;

    [Fact]
    public async Task Stream_HappyPath_MetaThenSentencesThenEnd()
    {
        _asr.NextText = "hello teacher";
        _chat.NextReply = "I love cats! They are soft.";   // 恰 2 句，护栏不截断
        _engine.NextResult = new byte[] { 1, 2, 3 };
        var device = Guid.NewGuid();

        var lines = await StreamRound(device);

        Assert.Equal(4, lines.Count);
        var meta = Obj(lines[0]);
        Assert.Equal("meta", Kind(meta));
        Assert.Equal("hello teacher", meta.GetProperty("transcript").GetString());
        Assert.Equal("qwen2.5:7b", meta.GetProperty("engine").GetString());
        Assert.Equal("free", meta.GetProperty("mode").GetString());
        Assert.NotEqual(Guid.Empty, meta.GetProperty("roundId").GetGuid());

        var s0 = Obj(lines[1]);
        Assert.Equal("s", Kind(s0));
        Assert.Equal(0, s0.GetProperty("i").GetInt32());
        Assert.Equal("I love cats!", s0.GetProperty("x").GetString());
        var u = s0.GetProperty("u").GetString()!;
        Assert.StartsWith("/api/device/audio/chat_", u);
        Assert.True(File.Exists(Path.Combine(_dir, Path.GetFileName(u))));
        Assert.Equal(1, Obj(lines[2]).GetProperty("i").GetInt32());
        Assert.Equal("They are soft.", Obj(lines[2]).GetProperty("x").GetString());

        var end = Obj(lines[3]);
        Assert.Equal("end", Kind(end));
        Assert.Equal("en", end.GetProperty("lang").GetString());
        Assert.Equal("neutral", end.GetProperty("emotion").GetString());
        Assert.Empty(end.GetProperty("commands").EnumerateArray());
        Assert.False(end.TryGetProperty("wordHits", out _));   // vocab 未注入 → 字段省略
        // 上下文回写 user+assistant 2 条；流式消息构建与老路径同源
        var stored = _redis.ReadStore<List<ChatMsg>>($"chat:{device:N}");
        Assert.Equal(2, stored!.Count);
        // 首轮无历史：system+user 两消息（流式与老路径同源构建）
        Assert.Equal(2, _chat.LastMessages!.Count);
        Assert.Equal(ChatRole.System, _chat.LastMessages[0].Role);
        Assert.Equal(ChatRole.User, _chat.LastMessages[1].Role);
    }

    [Fact]
    public async Task Stream_FirstSentenceBlocked_ReplacedWithSafeReply()
    {
        _asr.NextText = "tell me a story";
        _chat.NextReply = "Once there was a pirate with a gun. Then he ran away!";
        _engine.NextResult = new byte[] { 1 };

        var lines = await StreamRound(Guid.NewGuid());

        Assert.Equal("Sorry, let's talk about something fun! What is your favorite animal?",
            Obj(lines[1]).GetProperty("x").GetString());
        Assert.Equal("Then he ran away!", Obj(lines[2]).GetProperty("x").GetString());
        Assert.Equal("end", Kind(Obj(lines[^1])));
    }

    [Fact]
    public async Task Stream_LaterSentenceBlocked_DroppedAndTruncated()
    {
        _asr.NextText = "hi";
        _chat.NextReply = "Cats are cute. I have a gun here.";   // 第 2 句命中黑名单

        var lines = await StreamRound(Guid.NewGuid());

        Assert.Equal("Cats are cute.", Obj(lines[1]).GetProperty("x").GetString());
        Assert.DoesNotContain(1, lines.Where(l => Kind(Obj(l)) == "s")
            .Select(l => Obj(l).GetProperty("i").GetInt32()));   // 后续句丢弃截流：无 s1
        // 已发句照常收尾：end 仍发
        Assert.Equal("end", Kind(Obj(lines[^1])));
    }

    [Fact]
    public async Task Stream_WordLimit_TruncatesAtWordBoundary()
    {
        _asr.NextText = "hi";
        var longTail = string.Join(' ', Enumerable.Repeat("word", 45));
        _chat.NextReply = $"Hi! {longTail}. Final tail sentence here.";

        var lines = await StreamRound(Guid.NewGuid());

        var sLines = lines.Where(l => Kind(Obj(l)) == "s").ToList();
        Assert.Equal(2, sLines.Count);
        Assert.Equal("Hi!", Obj(sLines[0]).GetProperty("x").GetString());
        Assert.EndsWith("…", Obj(sLines[1]).GetProperty("x").GetString());   // 词边界截断
        // 截流：长句截断后后续 token 丢弃，尾句不再产出 s2
        Assert.Equal("end", Kind(Obj(lines[^1])));
    }

    [Fact]
    public async Task Stream_TtsFailure_SentencesCarryNoUrl()
    {
        _asr.NextText = "hi";
        _chat.NextReply = "Hello! Nice to see you.";
        _engine.NextResult = null;   // 合成失败

        var lines = await StreamRound(Guid.NewGuid());

        var sLines = lines.Where(l => Kind(Obj(l)) == "s").ToList();
        Assert.Equal(2, sLines.Count);
        Assert.All(sLines, l => Assert.False(Obj(l).TryGetProperty("u", out _)));   // null 降级字段省略
        Assert.Equal("end", Kind(Obj(lines[^1])));
    }

    [Fact]
    public async Task Stream_EmptyReply_EmitsErr()
    {
        _asr.NextText = "hi";
        _chat.NextReply = "";
        var device = Guid.NewGuid();

        var lines = await StreamRound(device);

        Assert.Equal(2, lines.Count);
        var err = Obj(lines[1]);
        Assert.Equal("err", Kind(err));
        Assert.Equal(502, err.GetProperty("code").GetInt32());
        Assert.Equal("empty llm reply", err.GetProperty("m").GetString());
        Assert.Null(_redis.ReadStore<List<ChatMsg>>($"chat:{device:N}"));   // err 路径不回写
    }

    [Fact]
    public async Task Stream_LlmMidStreamError_EmitsErrAfterEmittedSentences()
    {
        _asr.NextText = "hi";
        _chat.NextReply = "Hi there. More text is coming now.";
        _chat.ChunkSize = 10;
        _chat.StreamErrorAt = 10;   // 首句已产出后抛
        _chat.StreamError = new InvalidOperationException("ollama crashed");

        var lines = await StreamRound(Guid.NewGuid());

        Assert.Equal(3, lines.Count);
        Assert.Equal("Hi there.", Obj(lines[1]).GetProperty("x").GetString());
        var err = Obj(lines[2]);
        Assert.Equal("err", Kind(err));
        Assert.Equal(502, err.GetProperty("code").GetInt32());
    }

    [Fact]
    public async Task Stream_AbortMidRound_StopsSilently()
    {
        _asr.NextText = "hi";
        _chat.NextReply = "Hello there my friend! How are you today?";
        _chat.ChunkSize = 4;
        _chat.StreamDelayMs = 15;   // 首“My句 6 片×15ms ≈90ms，留足 abort 窗口
        var device = Guid.NewGuid();
        var lines = new List<string>();
        Guid roundId = Guid.Empty;

        await _svc.ConverseStreamAsync(device, Wav(1600), null, null,
            l =>
            {
                lines.Add(l);
                var e = Obj(l);
                if (Kind(e) == "meta")
                {
                    roundId = e.GetProperty("roundId").GetGuid();
                    _ = Task.Run(async () => { await Task.Delay(20); _svc.AbortRound(roundId); });
                }
                return Task.CompletedTask;
            }, CancellationToken.None);

        Assert.Equal("meta", Kind(Obj(lines[0])));
        Assert.DoesNotContain("end", lines.Select(l => Kind(Obj(l))));   // 静默终止无 end
        Assert.Null(_redis.ReadStore<List<ChatMsg>>($"chat:{device:N}"));   // 未回写上下文
        _svc.AbortRound(roundId);   // 注册表已清理：no-op 不炸
    }

    [Fact]
    public async Task Stream_Translate_ChineseSkippedEnglishTtsOnly()
    {
        _asr.NextText = "我喜欢猫";
        _chat.NextReply = "你喜欢猫。\nYou like cats.";
        _engine.NextResult = new byte[] { 1 };
        var device = Guid.NewGuid();

        var lines = await StreamRound(device, mode: "translate");

        var sLines = lines.Where(l => Kind(Obj(l)) == "s").ToList();
        Assert.Equal(2, sLines.Count);
        Assert.False(Obj(sLines[0]).TryGetProperty("u", out _));   // 中文句不合成
        Assert.True(Obj(sLines[1]).TryGetProperty("u", out _));    // 首个英文句合成
        Assert.Equal("en", _engine.LastLang);
        Assert.Equal(1, _engine.Calls);
        Assert.Equal("translate", Obj(lines[0]).GetProperty("mode").GetString());
        Assert.Equal("zh", Obj(lines[^1]).GetProperty("lang").GetString());
        var stored = _redis.ReadStore<List<ChatMsg>>($"chat:{device:N}:t");
        Assert.Equal(2, stored!.Count);
        Assert.Contains('\n', stored[1].Text);   // translate 两行 join 语义同源
    }

    [Fact]
    public async Task Stream_EndEmotion_PraisingKeyword()
    {
        _asr.NextText = "good";
        _chat.NextReply = "Great job! You know so many words.";
        _engine.NextResult = null;

        var lines = await StreamRound(Guid.NewGuid());

        Assert.Equal("praising", Obj(lines[^1]).GetProperty("emotion").GetString());
    }

    [Theory]
    [InlineData("again")]
    [InlineData("Again.")]
    [InlineData("say it again")]
    [InlineData("Pardon?")]
    public async Task Stream_ReplayCommand_ShortRetranscripts(string transcript)
    {
        _asr.NextText = transcript;
        _chat.NextReply = "You like cats. They are soft.";
        _engine.NextResult = null;

        var lines = await StreamRound(Guid.NewGuid());

        var cmds = Obj(lines[^1]).GetProperty("commands");
        Assert.Single(cmds.EnumerateArray());
        Assert.Equal("replay", cmds[0].GetProperty("a").GetString());
    }

    [Fact]
    public async Task Stream_ReplayCommand_NormalTranscript_Empty()
    {
        _asr.NextText = "what is your favorite animal again maybe";
        _chat.NextReply = "I love cats.";
        _engine.NextResult = null;

        var lines = await StreamRound(Guid.NewGuid());

        var cmds = Obj(lines[^1]).GetProperty("commands");
        Assert.Empty(cmds.EnumerateArray());   // 整句匹配：长句不误触
    }

    [Fact]
    public async Task Stream_AsrFailure_ThrowsBeforeFirstLine()
    {
        _asr.Mode = 0;

        var ex = await Assert.ThrowsAsync<ChatException>(() => StreamRound(Guid.NewGuid()));

        Assert.Equal(503, ex.StatusCode);
    }

    [Fact]
    public async Task Stream_HistoryOverflow_CompactsViaSummary()
    {
        _asr.NextText = "hello again";
        _chat.NextReply = "I love cats! They are soft.";
        _engine.NextResult = new byte[] { 1, 2, 3 };
        var device = Guid.NewGuid();
        var history = Enumerable.Range(0, 23)
            .Select(i => new ChatMsg(i % 2 == 0 ? "user" : "assistant", $"old {i}"))
            .ToList();
        _redis.WriteStore($"chat:{device:N}", history);

        var lines = await StreamRound(device);

        Assert.Equal("end", Kind(Obj(lines[^1])));   // 对话不因摘要压缩阻断
        var stored = _redis.ReadStore<List<ChatMsg>>($"chat:{device:N}")!;
        Assert.Equal(14, stored.Count);               // 25 → 1 摘要 + 13 近因
        Assert.StartsWith("[Earlier chat summary] ", stored[0].Text);
        Assert.Contains("cats", stored[0].Text);      // 摘要即 FakeChat 回复文本
    }

    [Fact]
    public async Task Stream_SummaryFailure_DegradesToTrim()
    {
        _asr.NextText = "hello again";
        _chat.NextReply = "I love cats! They are soft.";
        _engine.NextResult = new byte[] { 1, 2, 3 };
        _chat.FailAtNonStreamCall = 1;                 // 摘要调用（首次非流式）失败
        var device = Guid.NewGuid();
        var history = Enumerable.Range(0, 23)
            .Select(i => new ChatMsg(i % 2 == 0 ? "user" : "assistant", $"old {i}"))
            .ToList();
        _redis.WriteStore($"chat:{device:N}", history);

        var lines = await StreamRound(device);

        Assert.Equal("end", Kind(Obj(lines[^1])));   // 摘要失败不阻断对话
        var stored = _redis.ReadStore<List<ChatMsg>>($"chat:{device:N}")!;
        Assert.Equal(24, stored.Count);               // 降级：25 裁回 24（原裁剪路径）
        Assert.Equal("old 1", stored[0].Text);        // 直接裁剪，无摘要前缀
    }

    // ---- helpers ----

    /// <summary>最小合法 WAV（16kHz/16bit/mono，0.1s）</summary>
    private static byte[] Wav(int sampleCount) =>
        StandardWav(Enumerable.Repeat((short)8000, sampleCount).ToArray());

    private static byte[] StandardWav(short[] samples)
    {
        var data = new byte[samples.Length * 2];
        Buffer.BlockCopy(samples, 0, data, 0, data.Length);
        using var ms = new MemoryStream();
        using var w = new BinaryWriter(ms);
        w.Write("RIFF"u8);
        w.Write(36 + data.Length);
        w.Write("WAVE"u8);
        w.Write("fmt "u8);
        w.Write(16);
        w.Write((short)1);
        w.Write((short)1);
        w.Write(16_000);
        w.Write(32_000);
        w.Write((short)2);
        w.Write((short)16);
        w.Write("data"u8);
        w.Write(data.Length);
        w.Write(data);
        return ms.ToArray();
    }

    // ---- 替身 ----

    private sealed class FakeAsr : IAsrTranscriber
    {
        /// <summary>0=不可用(null) 1=空转写 2=正常</summary>
        public int Mode = 2;
        public string NextText = "hello";
        public int Calls;

        public string? Transcribe(short[] samples)
        {
            Calls++;
            return Mode switch
            {
                0 => null,
                1 => "",
                _ => NextText,
            };
        }
    }

    private sealed class FakeChat : IChatClient
    {
        public string? NextReply;
        public Exception? NextError;
        public int Calls;                    // GetResponseAsync 调用计数（P1-2 摘要失败注入用）
        public int FailAtNonStreamCall = -1; // 第 N 次（1 起）非流式调用抛（摘要失败降级路径）
        public List<ChatMessage>? LastMessages;
        public ChatOptions? LastOptions;

        public Task<ChatResponse> GetResponseAsync(IEnumerable<ChatMessage> messages,
            ChatOptions? options = null, CancellationToken cancellationToken = default)
        {
            Calls++;
            if (NextError is not null) throw NextError;
            if (Calls == FailAtNonStreamCall)
                throw new InvalidOperationException("injected non-stream failure");
            LastMessages = messages.ToList();
            LastOptions = options;
            if (NextReply is null) throw new InvalidOperationException("NextReply 未设置");
            return Task.FromResult(new ChatResponse(
                new ChatMessage(ChatRole.Assistant, NextReply)));
        }

        /// <summary>流式产出粒度/节奏/中途故障注入（P0-1）：ChunkSize
        /// 字符一片、片间 StreamDelayMs（abort 用例留打断窗口）、产出至
        /// StreamErrorAt 字符起抛 StreamError（模拟 LLM 中途崩）</summary>
        public int ChunkSize = 4;
        public int StreamDelayMs;
        public int StreamErrorAt = -1;
        public Exception? StreamError;

        public IAsyncEnumerable<ChatResponseUpdate> GetStreamingResponseAsync(
            IEnumerable<ChatMessage> messages, ChatOptions? options = null,
            CancellationToken cancellationToken = default) =>
            StreamCore(messages, options, cancellationToken);

        private async IAsyncEnumerable<ChatResponseUpdate> StreamCore(
            IEnumerable<ChatMessage> messages, ChatOptions? options,
            [EnumeratorCancellation] CancellationToken ct)
        {
            if (NextError is not null) throw NextError;
            LastMessages = messages.ToList();
            LastOptions = options;
            if (NextReply is null) throw new InvalidOperationException("NextReply 未设置");
            for (var i = 0; i < NextReply.Length; i += ChunkSize)
            {
                if (StreamDelayMs > 0) await Task.Delay(StreamDelayMs, ct);
                ct.ThrowIfCancellationRequested();
                if (StreamErrorAt >= 0 && i >= StreamErrorAt) throw StreamError!;
                yield return new ChatResponseUpdate(ChatRole.Assistant,
                    NextReply.Substring(i, Math.Min(ChunkSize, NextReply.Length - i)));
            }
        }

        public TService? GetService<TService>(object? serviceKey = null) where TService : class => null;

        public object? GetService(Type serviceType, object? serviceKey = null) => null;

        public void Dispose() { }
    }

    private sealed class FakeRedis : IRedisCache
    {
        private readonly Dictionary<string, object> _store = new();
        public bool Fail;

        public Dictionary<string, object> ReadStore() => _store;
        public T? ReadStore<T>(string key) => _store.TryGetValue(key, out var v) && v is T t ? t : default;
        public void WriteStore(string key, object value) => _store[key] = value;

        public Task<T?> GetAsync<T>(string key, CancellationToken ct = default)
        {
            if (Fail) throw new InvalidOperationException("redis down");
            return Task.FromResult(ReadStore<T>(key));
        }

        public async Task<T> GetOrSetAsync<T>(string key, Func<Task<T>> factory,
            TimeSpan? expiry = null, CancellationToken ct = default)
        {
            if (ReadStore<T>(key) is { } hit) return hit;
            var value = await factory();
            SetStore(key, value!);
            return value;
        }

        public Task SetAsync<T>(string key, T value, TimeSpan? expiry = null, CancellationToken ct = default)
        {
            if (Fail) throw new InvalidOperationException("redis down");
            SetStore(key, value!);
            return Task.CompletedTask;
        }

        public Task RemoveAsync(string key, CancellationToken ct = default)
        {
            _store.Remove(key);
            return Task.CompletedTask;
        }

        private void SetStore(string key, object value)
        {
            if (Fail) throw new InvalidOperationException("redis down");
            _store[key] = value;
        }
    }

    /// <summary>假合成引擎（TtsServiceTests 同款；LastText/LastLang 供 A2 语言路由断言）</summary>
    private sealed class FakeSynthesizer : ISpeechSynthesizer
    {
        public byte[]? NextResult;
        public int Calls;
        public string? LastText;
        public string? LastLang;

        public Task<byte[]?> SynthesizeMp3Async(string text, string lang, CancellationToken ct)
        {
            Calls++;
            LastText = text;
            LastLang = lang;
            return Task.FromResult(NextResult);
        }
    }

    /// <summary>A3 替身：记录 Enqueue 的对话轮（Hangfire sink 不入测试）</summary>
    private sealed class FakeTurnSink : IChatTurnSink
    {
        public List<ChatTurn> Turns { get; } = new();
        public void Enqueue(ChatTurn turn) => Turns.Add(turn);
    }

    /// <summary>A3 替身：固定词库快照</summary>
    private sealed class FakeVocab(IReadOnlyList<VocabEntry> words) : IWordListProvider
    {
        public ValueTask<IReadOnlyList<VocabEntry>> GetWordsAsync(CancellationToken ct) =>
            ValueTask.FromResult(words);
    }
}
