using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.Logging;
using SherpaOnnx;

namespace InkWord.Services;

/// <summary>ASR 转写抽象：输入 16kHz/16bit/mono 样本，输出文本；引擎不可用返回 null</summary>
public interface IAsrTranscriber
{
    string? Transcribe(short[] samples);
}

/// <summary>
/// sherpa-onnx zipformer 英语非流式转写（P2A，2026-08-24）。
///
/// org.k2fsa.sherpa.onnx C# 绑定（native 库随 NuGet 分发，容器内免装），
/// 模型文件走 volume 挂载（docker-compose sherpa:/data/sherpa）：
/// zipformer-en 三件套 encoder/decoder/joiner + tokens.txt。
/// OfflineRecognizer 惰性单例加载（秒级、只试一次），Decode 全程 lock 串行
/// （绑定层线程安全未承诺，单设备低频对话足够）。Asr:Provider=none 或模型
/// 缺失时降级返回 null，对话端点转 503——与 piper 不可用时 TtsJob 空转
/// 同款降级纪律，不炸启动。M5 GOP 升级共用本绑定（一次集成两处受益，
/// 见 docs/AI_SPEECH_ASSESSMENT.md）。
/// </summary>
public class SherpaAsrService : IAsrTranscriber
{
    private const int SampleRate = 16_000;

    private readonly object _lock = new();
    private readonly ILogger<SherpaAsrService> _logger;
    private readonly bool _enabled;
    private readonly string _tokens, _encoder, _decoder, _joiner;
    private readonly int _numThreads;
    private OfflineRecognizer? _recognizer;
    private bool _loadTried;

    public SherpaAsrService(IConfiguration cfg, ILogger<SherpaAsrService> logger)
    {
        _logger = logger;
        _enabled = string.Equals(cfg["Asr:Provider"], "sherpa", StringComparison.OrdinalIgnoreCase);
        var dir = cfg["Asr:ModelDir"] ?? "";
        _tokens = Path.Combine(dir, cfg["Asr:Tokens"] ?? "tokens.txt");
        _encoder = Path.Combine(dir, cfg["Asr:Encoder"] ?? "encoder-epoch-99-avg-1.onnx");
        _decoder = Path.Combine(dir, cfg["Asr:Decoder"] ?? "decoder-epoch-99-avg-1.onnx");
        _joiner = Path.Combine(dir, cfg["Asr:Joiner"] ?? "joiner-epoch-99-avg-1.onnx");
        if (!int.TryParse(cfg["Asr:NumThreads"], out _numThreads) || _numThreads <= 0)
            _numThreads = 1;
    }

    public string? Transcribe(short[] samples)
    {
        if (samples.Length == 0) return "";

        var recognizer = GetRecognizer();
        if (recognizer is null) return null;

        lock (_lock)
        {
            using var stream = recognizer.CreateStream();
            stream.AcceptWaveform(SampleRate, ToFloat(samples));
            recognizer.Decode(stream);
            return stream.Result.Text?.Trim();
        }
    }

    /// <summary>惰性加载 recognizer：失败/缺模型只试一次（对话端点持续 503 即信号，不刷日志）</summary>
    private OfflineRecognizer? GetRecognizer()
    {
        if (!_enabled) return null;
        lock (_lock)
        {
            if (_recognizer is not null || _loadTried) return _recognizer;
            _loadTried = true;
            if (!File.Exists(_tokens) || !File.Exists(_encoder))
            {
                _logger.LogWarning("ASR 模型缺失（Asr:ModelDir 未挂载?），chat 端点将 503：{Dir}",
                    Path.GetDirectoryName(_tokens));
                return null;
            }
            try
            {
                var config = new OfflineRecognizerConfig();
                config.FeatConfig.SampleRate = SampleRate;
                config.FeatConfig.FeatureDim = 80;
                config.ModelConfig.Tokens = _tokens;
                config.ModelConfig.Transducer.Encoder = _encoder;
                config.ModelConfig.Transducer.Decoder = _decoder;
                config.ModelConfig.Transducer.Joiner = _joiner;
                config.ModelConfig.NumThreads = _numThreads;
                config.ModelConfig.Debug = 0;
                config.DecodingMethod = "greedy_search";
                _recognizer = new OfflineRecognizer(config);
                _logger.LogInformation("sherpa-onnx ASR 就绪（zipformer-en，{Threads} 线程）", _numThreads);
            }
            catch (Exception ex)
            {
                _logger.LogWarning(ex, "ASR 初始化失败（native 库/模型不兼容?）");
            }
            return _recognizer;
        }
    }

    private static float[] ToFloat(short[] samples)
    {
        var f = new float[samples.Length];
        for (var i = 0; i < samples.Length; i++) f[i] = samples[i] / 32768f;
        return f;
    }
}
