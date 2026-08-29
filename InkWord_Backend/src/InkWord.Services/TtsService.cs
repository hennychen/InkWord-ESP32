using System.Diagnostics;
using System.Text.RegularExpressions;
using InkWord.Core.Entities;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.Logging;

namespace InkWord.Services;

/// <summary>
/// 语音合成引擎抽象（P0B，2026-08-24）：Piper 本地 / OpenAI 兼容云 API
/// 双态可切换（Tts:Provider），与 Ai:Provider 的 IChatClient 装配同模式。
/// </summary>
public interface ISpeechSynthesizer
{
    /// <summary>合成文本为 MP3 字节（lang="en"|"zh" 选音色，A2 中文路由）；
    /// 失败返回 null（调用方决定跳过/重试）</summary>
    Task<byte[]?> SynthesizeMp3Async(string text, string lang, CancellationToken ct);
}

/// <summary>
/// Piper 本地合成（默认引擎）：外部进程 piper(WAV) + ffmpeg(转 MP3 32k mono)。
/// 单词条 ≈4KB；qwen 例句 ≤6KB。CPU 单核亚秒级，批量 Job 串行调用即可。
/// </summary>
public partial class PiperSynthesizer : ISpeechSynthesizer
{
    private readonly string _piperPath;
    private readonly string _voice;
    private readonly string _voiceZh;
    private readonly string _ffmpegPath;
    private readonly ILogger<PiperSynthesizer> _logger;

    public PiperSynthesizer(IConfiguration cfg, ILogger<PiperSynthesizer> logger)
    {
        _piperPath = cfg["Tts:PiperPath"] ?? "piper";
        _voice = cfg["Tts:PiperVoice"] ?? "en_US-lessac-medium.onnx";
        _voiceZh = cfg["Tts:PiperVoiceZh"] ?? "zh_CN-huayan-medium.onnx";
        _ffmpegPath = cfg["Tts:FfmpegPath"] ?? "ffmpeg";
        _logger = logger;
    }

    public async Task<byte[]?> SynthesizeMp3Async(string text, string lang, CancellationToken ct)
    {
        if (string.IsNullOrWhiteSpace(text)) return null;
        var voice = lang == "zh" ? _voiceZh : _voice;   // A2 中文音色路由

        var wavPath = Path.Combine(Path.GetTempPath(), $"inkword-tts-{Guid.NewGuid():N}.wav");
        try
        {
            // 1) piper：stdin 喂文本，输出 WAV 文件
            var piper = ProcessStartInfoFor($"{Quote(_piperPath)} --voice {Quote(voice)} --output_file {Quote(wavPath)}");
            using (var proc = Process.Start(piper)!)
            {
                await proc.StandardInput.WriteLineAsync(text.AsMemory(), ct);
                await proc.StandardInput.FlushAsync(ct);
                proc.StandardInput.Close();
                // piper 读到 EOF 才收尾；超时 30s 防挂死
                using var killCts = CancellationTokenSource.CreateLinkedTokenSource(ct);
                killCts.CancelAfter(TimeSpan.FromSeconds(30));
                try { await proc.WaitForExitAsync(killCts.Token); }
                catch (OperationCanceledException) { TryKill(proc); return null; }
                if (proc.ExitCode != 0)
                {
                    _logger.LogWarning("piper 退出码 {Code}", proc.ExitCode);
                    return null;
                }
            }
            if (!File.Exists(wavPath) || new FileInfo(wavPath).Length == 0) return null;

            // 2) ffmpeg：WAV → MP3(32kbps mono) 到 stdout
            var ffmpeg = ProcessStartInfoFor(
                $"{Quote(_ffmpegPath)} -y -i {Quote(wavPath)} -codec:a libmp3lame -b:a 32k -ac 1 -f mp3 pipe:1");
            using (var proc = Process.Start(ffmpeg)!)
            using (var ms = new MemoryStream())
            {
                await proc.StandardOutput.BaseStream.CopyToAsync(ms, ct);
                await proc.WaitForExitAsync(ct);
                if (proc.ExitCode != 0 || ms.Length == 0)
                {
                    _logger.LogWarning("ffmpeg 退出码 {Code}", proc.ExitCode);
                    return null;
                }
                return ms.ToArray();
            }
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex, "piper 合成异常（二进制未安装/模型缺失?）");
            return null;
        }
        finally
        {
            try { if (File.Exists(wavPath)) File.Delete(wavPath); } catch { /* 临时文件尽力清理 */ }
        }
    }

    private ProcessStartInfo ProcessStartInfoFor(string arguments)
    {
        // arguments 按整串拼接（路径含空格已 Quote），shell 交由 OS 解析
        var info = new ProcessStartInfo
        {
            FileName = "/bin/sh",
            Arguments = $"-c {Quote(arguments)}",
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        return info;
    }

    private static string Quote(string s) => $"\"{s}\"";

    private static void TryKill(Process proc)
    {
        try { proc.Kill(entireProcessTree: true); } catch { /* 已退出 */ }
    }
}

/// <summary>
/// TTS 编排服务（P0B，2026-08-24）：词条音频「缓存资产」的存取与下发白名单。
///
/// 存储约定：Tts:AudioDir（默认 data/audio）下 {wordId:N}.mp3——
/// **不回填 Word.Audio 字段、不 bump Version**，与词库增量同步完全解耦；
/// 设备端由 audio_sync 按 words.json cloudId 推导文件名补齐 SD 卡。
/// 对话临时音频（P2A）用 chat_ 前缀命名，由清理 Job 定期回收。
/// </summary>
public partial class TtsService
{
    /// <summary>送入合成引擎的文本长度护栏（词条/短句场景）</summary>
    public const int MaxTextLength = 300;

    // 下发白名单：小写字母数字与连字符 + .mp3，禁路径分隔符（防穿越）
    [GeneratedRegex(@"^[A-Za-z0-9_\-]+\.mp3$")]
    private static partial Regex SafeFileNameRegex();

    private readonly ISpeechSynthesizer _engine;
    private readonly ILogger<TtsService> _logger;
    private readonly string _audioDir;

    public TtsService(ISpeechSynthesizer engine, IConfiguration cfg, ILogger<TtsService> logger)
    {
        _engine = engine;
        _logger = logger;
        _audioDir = cfg["Tts:AudioDir"] ?? "data/audio";
        Directory.CreateDirectory(_audioDir);
    }

    public string AudioDir => _audioDir;

    public static string WordFileName(Guid wordId) => $"{wordId:N}.mp3";

    public string WordFilePath(Guid wordId) => Path.Combine(_audioDir, WordFileName(wordId));

    public bool HasAudio(Guid wordId) => File.Exists(WordFilePath(wordId));

    /// <summary>词条发音：已缓存直接返回；否则合成 word.Text 落盘（幂等）</summary>
    public async Task<bool> EnsureWordAudioAsync(Word word, CancellationToken ct)
    {
        if (HasAudio(word.Id)) return true;
        var mp3 = await _engine.SynthesizeMp3Async(Sanitize(word.Text), "en", ct);
        if (mp3 is null || mp3.Length == 0)
        {
            _logger.LogDebug("词条合成失败：{Word}", word.Text);
            return false;
        }
        await WriteAtomicAsync(WordFileName(word.Id), mp3, ct);
        return true;
    }

    /// <summary>任意短文本合成并按指定文件名落盘（对话回复 chat_*.mp3；
    /// lang 透传引擎选音色，A2 中文路由）</summary>
    public async Task<string?> SaveClipAsync(string fileName, string text, string lang, CancellationToken ct)
    {
        if (!SafeFileNameRegex().IsMatch(fileName))
        {
            _logger.LogWarning("非法音频文件名被拒：{File}", fileName);
            return null;
        }
        var mp3 = await _engine.SynthesizeMp3Async(Sanitize(text), lang, ct);
        if (mp3 is null || mp3.Length == 0) return null;
        await WriteAtomicAsync(fileName, mp3, ct);
        return fileName;
    }

    /// <summary>下发白名单校验：仅本目录内、无路径分隔符的 .mp3 文件名</summary>
    public bool TryResolveSafePath(string fileName, out string fullPath)
    {
        fullPath = "";
        if (!SafeFileNameRegex().IsMatch(fileName)) return false;
        var candidate = Path.GetFullPath(Path.Combine(_audioDir, fileName));
        if (!candidate.StartsWith(Path.GetFullPath(_audioDir) + Path.DirectorySeparatorChar))
            return false;  // 双保险：拼接后仍须落在音频目录内
        fullPath = candidate;
        return true;
    }

    /// <summary>清理过期对话音频（P2A 回收 Job 用）：前缀 chat_、mtime 早于阈值</summary>
    public int CleanupChatClips(TimeSpan olderThan)
    {
        var cutoff = DateTime.UtcNow - olderThan;
        var removed = 0;
        foreach (var file in Directory.EnumerateFiles(_audioDir, "chat_*.mp3"))
        {
            try
            {
                if (File.GetLastWriteTimeUtc(file) < cutoff)
                {
                    File.Delete(file);
                    removed++;
                }
            }
            catch (IOException) { /* 并发占用跳过 */ }
        }
        return removed;
    }

    private async Task WriteAtomicAsync(string fileName, byte[] bytes, CancellationToken ct)
    {
        var finalPath = Path.Combine(_audioDir, fileName);
        var tmpPath = finalPath + ".tmp";
        await File.WriteAllBytesAsync(tmpPath, bytes, ct);
        File.Move(tmpPath, finalPath, overwrite: true);  // 半文件防护：完成后原子改名
    }

    /// <summary>合成文本清洗：去控制字符、限长（词条里的音标斜杠等原样保留）</summary>
    public static string Sanitize(string text)
    {
        var clean = new string(text.Where(c => !char.IsControl(c)).ToArray()).Trim();
        return clean.Length <= MaxTextLength ? clean : clean[..MaxTextLength];
    }
}
