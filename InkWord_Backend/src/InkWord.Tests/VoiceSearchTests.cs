using InkWord.Services;
using Microsoft.Extensions.Logging.Abstractions;

namespace InkWord.Tests;

/// <summary>
/// 语音查词纯逻辑测试（2026-08-28，设计 §测试计划）。
///
/// 覆盖：文本归一 / 三层匹配打分 / 编辑距离边界；SearchAsync 的
/// 400（坏 WAV）/503（ASR 不可用）/400（无话音）三条错误路径均在
/// DbContext 触碰前抛出，以 null db 直测无需 EF 替身。命中排序与
/// deck 范围过滤留端点级 curl 验证（DeckShareTests 缺口口径同款）。
/// </summary>
public class VoiceSearchTests
{
    private static VoiceSearchService Svc(IAsrTranscriber asr) =>
        new(asr, null!, NullLogger<VoiceSearchService>.Instance);

    private sealed class StubAsr : IAsrTranscriber
    {
        private readonly string? _result;
        public StubAsr(string? result) => _result = result;
        public string? Transcribe(short[] samples) => _result;
    }

    /// <summary>标准 44B 头 WAV（16kHz/16bit/mono）+ sampleCount 个零样本</summary>
    private static byte[] MakeWav(int sampleCount)
    {
        int dataLen = sampleCount * 2;
        byte[] wav = new byte[44 + dataLen];
        byte[] tag = "RIFF WAVEfmt data"u8.ToArray();   // RIFF[0..3] WAVE[5..8] "fmt "[9..12] data[13..16]
        Array.Copy(tag, 0, wav, 0, 4);
        BitConverter.GetBytes(36 + dataLen).CopyTo(wav, 4);
        Array.Copy(tag, 5, wav, 8, 4);
        Array.Copy(tag, 9, wav, 12, 4);
        BitConverter.GetBytes(16).CopyTo(wav, 16);        // fmt size
        BitConverter.GetBytes((short)1).CopyTo(wav, 20); // PCM
        BitConverter.GetBytes((short)1).CopyTo(wav, 22);  // mono
        BitConverter.GetBytes(16_000).CopyTo(wav, 24);
        BitConverter.GetBytes(32_000).CopyTo(wav, 28);    // byte rate
        BitConverter.GetBytes((short)2).CopyTo(wav, 32);  // block align
        BitConverter.GetBytes((short)16).CopyTo(wav, 34);
        Array.Copy(tag, 13, wav, 36, 4);
        BitConverter.GetBytes(dataLen).CopyTo(wav, 40);
        return wav;
    }

    // ---- Normalize ----

    [Theory]
    [InlineData("Apple", "apple")]
    [InlineData("  THE   apple ", "the apple")]
    [InlineData("apple, please!", "apple please")]
    [InlineData("how are you?", "how are you")]
    [InlineData("（yes）", "yes")]
    [InlineData("", "")]
    public void Normalize_StripsAndLowercases(string raw, string expected) =>
        Assert.Equal(expected, VoiceSearchService.Normalize(raw));

    // ---- ScoreWord 分层 ----

    [Fact]
    public void ScoreWord_L1_ExactAfterNormalize()
    {
        Assert.Equal(1.0, VoiceSearchService.ScoreWord("apple", "Apple"));
        Assert.Equal(1.0, VoiceSearchService.ScoreWord("how are you", "How Are You"));
    }

    [Fact]
    public void ScoreWord_L2_PrefixOrContains_Bidirectional()
    {
        // ASR 复数漂移：transcript 比 word 长（前缀方向）
        Assert.Equal(0.7, VoiceSearchService.ScoreWord("apples", "apple"));
        // 冠词粘连：transcript 包含 word
        Assert.Equal(0.7, VoiceSearchService.ScoreWord("the apple", "apple"));
        // word 包含 transcript（前缀方向反例：word 更长且非前缀关系）
        Assert.Equal(0.7, VoiceSearchService.ScoreWord("spect", "inspection"));
    }

    [Fact]
    public void ScoreWord_L2_ShortSideBelow3_Rejected()
    {
        // 短边 <3 的包含命中拒绝（防 "a"/"an" 冠词误命中）
        Assert.Equal(0, VoiceSearchService.ScoreWord("a pen", "a"));
        Assert.Equal(0, VoiceSearchService.ScoreWord("an", "banana"));
    }

    [Fact]
    public void ScoreWord_L3_EditDistanceTolerance()
    {
        // ASR 拼写漂移（ability → abilty，1 处换位=编辑距离 2）
        Assert.Equal(0.4, VoiceSearchService.ScoreWord("abilty", "ability"));
        Assert.Equal(0.4, VoiceSearchService.ScoreWord("beatuiful", "beautiful"));
        // 词长 <5 不启用（cat→bat 距离 1 但拒绝）
        Assert.Equal(0, VoiceSearchService.ScoreWord("bat", "cat"));
        // 长度差 >2 拒绝
        Assert.Equal(0, VoiceSearchService.ScoreWord("abilitiest", "ability"));
    }

    [Fact]
    public void ScoreWord_EmptyInput_Zero()
    {
        Assert.Equal(0, VoiceSearchService.ScoreWord("", "apple"));
        Assert.Equal(0, VoiceSearchService.ScoreWord("apple", ""));
        Assert.Equal(0, VoiceSearchService.ScoreWord("", "!!!"));
    }

    // ---- EditDistance ----

    [Theory]
    [InlineData("apple", "apple", 0)]
    [InlineData("apple", "apples", 1)]
    [InlineData("abilty", "ability", 1)]     // 单点插入 i
    [InlineData("kitten", "sitting", 3)]
    [InlineData("", "abc", 3)]
    [InlineData("abc", "", 3)]
    public void EditDistance_Classic(string a, string b, int expected) =>
        Assert.Equal(expected, VoiceSearchService.EditDistance(a, b));

    // ---- SearchAsync 错误路径（DbContext 触碰前抛出，null db 可测） ----

    [Fact]
    public async Task SearchAsync_BadWav_Throws400()
    {
        await Assert.ThrowsAsync<VoiceSearchException>(() =>
            Svc(new StubAsr("apple")).SearchAsync(new byte[8], null, default));
    }

    [Fact]
    public async Task SearchAsync_AsrUnavailable_Throws503()
    {
        var ex = await Assert.ThrowsAsync<VoiceSearchException>(() =>
            Svc(new StubAsr(null)).SearchAsync(MakeWav(100), null, default));
        Assert.Equal(503, ex.StatusCode);
    }

    [Fact]
    public async Task SearchAsync_NoSpeech_Throws400()
    {
        var ex = await Assert.ThrowsAsync<VoiceSearchException>(() =>
            Svc(new StubAsr("")).SearchAsync(MakeWav(100), null, default));
        Assert.Equal(400, ex.StatusCode);
    }

    [Fact]
    public async Task SearchAsync_WavTooLarge_Throws400()
    {
        var ex = await Assert.ThrowsAsync<VoiceSearchException>(() =>
            Svc(new StubAsr("apple"))
                .SearchAsync(new byte[VoiceSearchService.MaxWavBytes + 1], null, default));
        Assert.Equal(400, ex.StatusCode);
    }
}
