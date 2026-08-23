namespace InkWord.Services;

/// <summary>发音评测结果</summary>
/// <param name="Total">总分 0~100</param>
/// <param name="DurationMs">有效语音时长（毫秒）</param>
/// <param name="Engine">评分引擎标识（heuristic=过渡引擎 / gop=sherpa-onnx）</param>
/// <param name="Phonemes">音素级明细（GOP 引擎提供；过渡引擎为空）</param>
public record PronunciationScore(int Total, int DurationMs, string Engine,
    IReadOnlyList<PhonemeScore> Phonemes);

public record PhonemeScore(string Phoneme, int Score, float StartSec, float EndSec);

/// <summary>
/// 语音跟读评测服务（M5 路径 C，2026-08-22）。
///
/// 目标架构：sherpa-onnx GOP（音素强制对齐 + 后验概率）音素级评测，
/// 集成路径见 docs/AI_SPEECH_ASSESSMENT.md（模型 ~40MB volume 挂载）。
/// 在 native 库接入前，先以信号质量启发式引擎（话音时长占比 + 削顶/
/// 静默检测）提供可用闭环：它能区分「读满/没读/环境噪声」，不具备
/// 音素级辨析能力——Engine 字段显式标识，前端/固件据此降级提示。
/// 协议（WAV 16kHz/16bit/mono ≤3s + POST /api/device/pronunciation）先行
/// 冻结，固件端（INMP441 录音上传）按此对接。
/// </summary>
public class PronunciationService
{
    private const int SampleRate = 16_000;
    private const int MaxBytes = 3 * SampleRate * 2 + 64; // 3s 16bit ≈ 96KB

    /// <summary>
    /// 评测一段跟读音频。wav 为原始 WAV 字节（RIFF/PCM 16bit/mono/16kHz），
    /// wordText 为目标单词。格式不符抛 InvalidDataException（端点转 400）。
    /// </summary>
    public Task<PronunciationScore> AssessAsync(byte[] wav, string wordText, CancellationToken ct)
    {
        if (string.IsNullOrWhiteSpace(wordText))
            throw new InvalidDataException("wordText required");
        var samples = ParsePcm(wav);

        var score = ScoreHeuristic(samples, wordText);
        return Task.FromResult(score);
    }

    // ---- WAV 解析（RIFF/PCM 16bit/mono/16kHz）----

    internal static short[] ParsePcm(byte[] wav)
    {
        if (wav.Length < 44 || wav[0] != 'R' || wav[1] != 'I' || wav[2] != 'F' || wav[3] != 'F')
            throw new InvalidDataException("not a RIFF/WAV file");
        if (wav.Length > MaxBytes)
            throw new InvalidDataException($"wav too large (>{MaxBytes / 1024}KB)");

        // fmt 块：快速路径限标准 44B 头（"fmt "@12, size@16=16, "data"@36）；
        // 含额外块（LIST/fact）或非标 fmt 尺寸时按 RIFF 块遍历兑底
        short channels = 0, bits = 0;
        int sampleRate = 0, dataOffset = -1, dataLen = 0;
        if (BitConverter.ToInt32(wav, 16) == 16 && BitConverter.ToInt32(wav, 36) == 0x61746164)
        {
            channels = BitConverter.ToInt16(wav, 22);
            sampleRate = BitConverter.ToInt32(wav, 24);
            bits = BitConverter.ToInt16(wav, 34);
            dataOffset = 44;
            dataLen = BitConverter.ToInt32(wav, 40);
        }
        else
        {
            // RIFF 块遍历：pos=12（"WAVE" 后）起逐块读 fourCC+size，偶对齐跨块
            int pos = 12;
            while (pos + 8 <= wav.Length)
            {
                var id = BitConverter.ToInt32(wav, pos);
                int size = BitConverter.ToInt32(wav, pos + 4);
                if (id == 0x20746d66) // "fmt "
                {
                    channels = BitConverter.ToInt16(wav, pos + 10);
                    sampleRate = BitConverter.ToInt32(wav, pos + 12);
                    bits = BitConverter.ToInt16(wav, pos + 22);
                }
                else if (id == 0x61746164) // "data"
                {
                    dataOffset = pos + 8;
                    dataLen = size;
                    break;
                }
                pos += 8 + size + (size & 1); // RIFF 块偶对齐
            }
        }
        if (dataOffset < 0)
            throw new InvalidDataException("non-standard WAV chunk layout not supported");

        if (channels != 1 || bits != 16 || sampleRate != SampleRate)
            throw new InvalidDataException(
                $"require 16kHz/16bit/mono, got {sampleRate}Hz/{bits}bit/{channels}ch");

        dataLen = Math.Min(dataLen, wav.Length - dataOffset) & ~1; // 偶数对齐
        var samples = new short[dataLen / 2];
        Buffer.BlockCopy(wav, dataOffset, samples, 0, dataLen);
        return samples;
    }

    // ---- 过渡引擎：信号质量启发式（非 GOP，见类注释）----

    internal static PronunciationScore ScoreHeuristic(short[] samples, string wordText)
    {
        const int frameMs = 20;
        int frameLen = SampleRate * frameMs / 1000;
        int frames = samples.Length / frameLen;

        // 逐帧 RMS → 话音活性（阈值 = 整体峰值 -26dB；环境底噪通常低 30dB+）
        double peak = 1;
        for (var i = 0; i < samples.Length; i++)
            peak = Math.Max(peak, Math.Abs((double)samples[i]));
        var actThresh = peak * Math.Pow(10, -26.0 / 20.0);

        int activeFrames = 0, clipped = 0;
        for (var f = 0; f < frames; f++)
        {
            double sum = 0;
            for (var i = f * frameLen; i < (f + 1) * frameLen; i++)
            {
                var v = (double)samples[i];
                sum += v * v;
                if (Math.Abs(v) > 32000) clipped++;
            }
            if (Math.Sqrt(sum / frameLen) > actThresh) activeFrames++;
        }

        var durationMs = frames * frameMs;
        var activeRatio = frames == 0 ? 0 : (double)activeFrames / frames;
        var clippedRatio = samples.Length == 0 ? 0 : (double)clipped / samples.Length;

        // 时长期望：单词音节数近似 ceil(len/3)，每音节 150~350ms
        var syllables = Math.Max(1, (wordText.Length + 2) / 3);
        var expectMs = syllables * 250.0;
        var durScore = durationMs == 0 ? 0
            : 100.0 * Math.Exp(-Math.Pow((durationMs - expectMs) / (expectMs * 0.8), 2));

        // 评分 = 时长匹配 60% + 话音占比 25% + 无削顶 15%
        var total = (int)Math.Clamp(
            0.60 * durScore + 25.0 * Math.Min(1.0, activeRatio / 0.7) + 15.0 * (1.0 - Math.Min(1.0, clippedRatio * 10)),
            0, 100);

        return new PronunciationScore(total, durationMs, "heuristic", []);
    }
}
