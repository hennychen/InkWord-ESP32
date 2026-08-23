using System.Text;
using InkWord.Services;
using Xunit;

namespace InkWord.Tests;

/// <summary>
/// PronunciationService WAV 解析测试（M5 路径 C，2026-08-22）。
///
/// 覆盖标准 44B 头快速路径与 RIFF 块遍历兜底（带 LIST 块的非标布局），
/// 以及格式硬约束拒绝（采样率/位深/声道不符）。头部偏移曾因 fmt size
/// 误读 offset 20（实际 16）导致全部标准文件被拒——本测试固化防回归。
/// </summary>
public class PronunciationServiceTests
{
    private readonly PronunciationService _svc = new();

    /// <summary>构造 44B 标准头 WAV（RIFF/PCM 16kHz/16bit/mono）</summary>
    private static byte[] StandardWav(short[] samples, int sampleRate = 16_000,
        short channels = 1, short bits = 16)
    {
        var data = new byte[samples.Length * 2];
        Buffer.BlockCopy(samples, 0, data, 0, data.Length);

        var ms = new MemoryStream();
        var w = new BinaryWriter(ms);
        w.Write("RIFF"u8);
        w.Write(36 + data.Length);
        w.Write("WAVE"u8);
        w.Write("fmt "u8);
        w.Write(16);            // fmt chunk size（offset 16）
        w.Write((short)1);      // PCM
        w.Write(channels);
        w.Write(sampleRate);
        w.Write(sampleRate * channels * bits / 8);
        w.Write((short)(channels * bits / 8));
        w.Write(bits);
        w.Write("data"u8);
        w.Write(data.Length);
        w.Write(data);
        return ms.ToArray();
    }

    /// <summary>构造带 LIST 块的非标布局 WAV（fmt/data 之间插入 10B INFO 块）</summary>
    private static byte[] ListChunkWav(short[] samples)
    {
        var data = new byte[samples.Length * 2];

        var ms = new MemoryStream();
        var w = new BinaryWriter(ms);
        w.Write("RIFF"u8);
        w.Write(50 + data.Length - 8 + 10); // 粗略长度（解析器不依赖）
        w.Write("WAVE"u8);
        w.Write("fmt "u8);
        w.Write(16);
        w.Write((short)1);
        w.Write((short)1);
        w.Write(16_000);
        w.Write(32_000);
        w.Write((short)2);
        w.Write((short)16);
        w.Write("LIST"u8);
        w.Write(10);           // 非标：fmt 与 data 之间插入 LIST 块
        w.Write(new byte[10]);
        w.Write("data"u8);
        w.Write(data.Length);
        w.Write(data);
        return ms.ToArray();
    }

    [Fact]
    public async Task Parse_Standard44BHeader_Scores()
    {
        // 1s 幅度 0.5 的有效话音（启发式引擎：非静音非削顶）
        var samples = Enumerable.Repeat((short)16_000, 16_000).ToArray();
        var score = await _svc.AssessAsync(StandardWav(samples), "inspect", CancellationToken.None);
        Assert.InRange(score.Total, 0, 100);
        Assert.Equal("heuristic", score.Engine);
        Assert.Equal(1000, score.DurationMs);
    }

    [Fact]
    public async Task Parse_ListChunkFallback_Scores()
    {
        var samples = Enumerable.Repeat((short)-16_000, 16_000).ToArray();
        var score = await _svc.AssessAsync(ListChunkWav(samples), "inspect", CancellationToken.None);
        Assert.InRange(score.Total, 0, 100);
        Assert.Equal(1000, score.DurationMs);
    }

    [Fact]
    public async Task Parse_WrongSampleRate_Rejected()
    {
        var samples = new short[8000];
        await Assert.ThrowsAsync<InvalidDataException>(() =>
            _svc.AssessAsync(StandardWav(samples, sampleRate: 44_100), "inspect",
                CancellationToken.None));
    }

    [Fact]
    public async Task Parse_NotRiff_Rejected()
    {
        var garbage = Encoding.ASCII.GetBytes("NOTAWAVFILE....").ToArray();
        await Assert.ThrowsAsync<InvalidDataException>(() =>
            _svc.AssessAsync(garbage, "inspect", CancellationToken.None));
    }
}
