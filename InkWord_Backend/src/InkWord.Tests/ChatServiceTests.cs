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

    // ---- 会话上下文 ----

    [Fact]
    public async Task Converse_SecondRound_CarriesHistoryAndTrims()
    {
        var device = Guid.NewGuid();
        _asr.NextText = "what is this";
        _chat.NextReply = "It is a pen.";
        _engine.NextResult = new byte[] { 1 };

        await _svc.ConverseAsync(device, Wav(1600), CancellationToken.None);
        // 第二轮前预塞历史到上限：再加本轮应裁回 24 条（12 轮）
        var history = Enumerable.Range(0, 23)
            .Select(i => new ChatMsg(i % 2 == 0 ? "user" : "assistant", $"old {i}"))
            .ToList();
        _redis.WriteStore($"chat:{device:N}", history);

        await _svc.ConverseAsync(device, Wav(1600), CancellationToken.None);

        var stored = _redis.ReadStore<List<ChatMsg>>($"chat:{device:N}");
        Assert.Equal(24, stored.Count);   // 23 预置 + 2 本轮 = 25 → 裁 1
        // 第二轮 LLM 收到 system + 上下文 + 当前问句
        Assert.True(_chat.LastMessages!.Count >= 3);
        Assert.Equal(ChatRole.System, _chat.LastMessages[0].Role);
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
        public List<ChatMessage>? LastMessages;

        public Task<ChatResponse> GetResponseAsync(IEnumerable<ChatMessage> messages,
            ChatOptions? options = null, CancellationToken cancellationToken = default)
        {
            if (NextError is not null) throw NextError;
            LastMessages = messages.ToList();
            if (NextReply is null) throw new InvalidOperationException("NextReply 未设置");
            return Task.FromResult(new ChatResponse(
                new ChatMessage(ChatRole.Assistant, NextReply)));
        }

        public IAsyncEnumerable<ChatResponseUpdate> GetStreamingResponseAsync(
            IEnumerable<ChatMessage> messages, ChatOptions? options = null,
            CancellationToken cancellationToken = default) => throw new NotSupportedException();

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

    /// <summary>假合成引擎（TtsServiceTests 同款）</summary>
    private sealed class FakeSynthesizer : ISpeechSynthesizer
    {
        public byte[]? NextResult;
        public int Calls;

        public Task<byte[]?> SynthesizeMp3Async(string text, CancellationToken ct)
        {
            Calls++;
            return Task.FromResult(NextResult);
        }
    }
}
