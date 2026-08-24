using InkWord.Core.Entities;
using InkWord.Services;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.Logging.Abstractions;
using Xunit;

namespace InkWord.Tests;

/// <summary>
/// TtsService 编排逻辑测试（P0B，2026-08-24）。
///
/// ISpeechSynthesizer 用假引擎替身（真 piper/ffmpeg 属外部进程，集成验证）；
/// 重点覆盖：文件名白名单（路径穿越防护——设备端下发端点的安全边界）、
/// 幂等缓存、原子写与 chat_ 前缀回收。
/// </summary>
public class TtsServiceTests : IDisposable
{
    private readonly string _dir;
    private readonly TtsService _svc;
    private readonly FakeSynthesizer _engine = new();

    public TtsServiceTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), $"inkword-tts-test-{Guid.NewGuid():N}");
        var cfg = new ConfigurationBuilder()
            .AddInMemoryCollection(new Dictionary<string, string?>
            {
                ["Tts:AudioDir"] = _dir,
            })
            .Build();
        _svc = new TtsService(_engine, cfg, new NullLogger<TtsService>());
    }

    public void Dispose()
    {
        if (Directory.Exists(_dir)) Directory.Delete(_dir, recursive: true);
    }

    // ---- 文件名白名单（下发端点安全边界） ----

    [Theory]
    [InlineData("../secrets.mp3")]           // 路径穿越
    [InlineData("..\\secrets.mp3")]
    [InlineData("sub/dir/file.mp3")]          // 路径分隔符
    [InlineData("noext")]
    [InlineData("word.wav")]                  // 非 mp3
    [InlineData("")]
    [InlineData("a b.mp3")]                   // 空格
    [InlineData("话.mp3")]                    // 非 ASCII
    public void TryResolveSafePath_RejectsUnsafe(string name)
    {
        Assert.False(_svc.TryResolveSafePath(name, out _));
    }

    [Theory]
    [InlineData("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef.mp3")]  // {N} guid
    [InlineData("chat_1690000000.mp3")]
    public void TryResolveSafePath_AcceptsSafe(string name)
    {
        Assert.True(_svc.TryResolveSafePath(name, out var path));
        Assert.StartsWith(Path.GetFullPath(_dir), path);
    }

    // ---- 词条合成幂等 ----

    [Fact]
    public async Task EnsureWordAudio_SynthesizesOnce_ThenCached()
    {
        var word = new Word { Id = Guid.NewGuid(), Text = "hello" };
        _engine.NextResult = new byte[] { 0xFF, 0xFB, 0x90, 0x00 };  // MP3 帧头样子

        Assert.True(await _svc.EnsureWordAudioAsync(word, CancellationToken.None));
        Assert.True(await _svc.EnsureWordAudioAsync(word, CancellationToken.None));

        Assert.Equal(1, _engine.Calls);  // 第二次命中缓存，引擎零调用
        Assert.True(File.Exists(_svc.WordFilePath(word.Id)));
    }

    [Fact]
    public async Task EnsureWordAudio_EngineFailure_ReturnsFalseNoFile()
    {
        var word = new Word { Id = Guid.NewGuid(), Text = "hello" };
        _engine.NextResult = null;

        Assert.False(await _svc.EnsureWordAudioAsync(word, CancellationToken.None));
        Assert.False(_svc.HasAudio(word.Id));
    }

    // ---- 对话短音频 ----

    [Fact]
    public async Task SaveClip_WritesFile_AndRejectsUnsafeName()
    {
        _engine.NextResult = new byte[] { 1, 2, 3 };
        var saved = await _svc.SaveClipAsync("chat_123.mp3", "Good job!", CancellationToken.None);
        Assert.Equal("chat_123.mp3", saved);
        Assert.True(File.Exists(Path.Combine(_dir, "chat_123.mp3")));

        Assert.Null(await _svc.SaveClipAsync("../evil.mp3", "x", CancellationToken.None));
    }

    // ---- chat_ 回收 ----

    [Fact]
    public void CleanupChatClips_RemovesOnlyExpiredChatFiles()
    {
        var oldChat = Path.Combine(_dir, "chat_old.mp3");
        var newChat = Path.Combine(_dir, "chat_new.mp3");
        var wordFile = Path.Combine(_dir, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef.mp3");
        File.WriteAllText(oldChat, "x");
        File.WriteAllText(newChat, "x");
        File.WriteAllText(wordFile, "x");
        File.SetLastWriteTimeUtc(oldChat, DateTime.UtcNow.AddHours(-2));

        var removed = _svc.CleanupChatClips(TimeSpan.FromHours(1));

        Assert.Equal(1, removed);
        Assert.False(File.Exists(oldChat));
        Assert.True(File.Exists(newChat));   // 未过期保留
        Assert.True(File.Exists(wordFile));  // 词条音频不误删
    }

    // ---- 文本清洗 ----

    [Theory]
    [InlineData("hello\r\nworld", "hello\r\nworld")]  // \r\n 是控制字符，会被剥掉
    [InlineData("  spaced  ", "spaced")]
    public void Sanitize_StripsControlChars(string input, string expected)
    {
        // 注：\r\n 属控制字符 → 剥除后拼接
        var actual = TtsService.Sanitize(input);
        Assert.DoesNotContain('\r', actual);
        Assert.DoesNotContain('\n', actual);
        Assert.Equal(expected.Replace("\r\n", ""), actual);
    }

    [Fact]
    public void Sanitize_ClampsLength()
    {
        var longText = new string('a', TtsService.MaxTextLength + 50);
        Assert.Equal(TtsService.MaxTextLength, TtsService.Sanitize(longText).Length);
    }

    [Fact]
    public void WordFileName_IsGuidN()
    {
        var id = Guid.NewGuid();
        Assert.Equal($"{id:N}.mp3", TtsService.WordFileName(id));
        Assert.DoesNotContain('-', TtsService.WordFileName(id));
    }

    private sealed class FakeSynthesizer : ISpeechSynthesizer
    {
        public byte[]? NextResult { get; set; } = new byte[] { 0x01 };
        public int Calls { get; private set; }

        public Task<byte[]?> SynthesizeMp3Async(string text, CancellationToken ct)
        {
            Calls++;
            return Task.FromResult(NextResult);
        }
    }
}
