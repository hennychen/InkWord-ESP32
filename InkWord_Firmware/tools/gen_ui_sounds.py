#!/usr/bin/env python3
"""InkWord UI 提示音生成器 (v1.1 T1.6，2026-08-24)

生成 4 个提示音样本（16kHz / mono / PCM16 / <0.3s）：
  key.wav  按键按下确认 -- 短「滴」（1.8kHz 正弦指数衰减，70ms）
  rate.wav 自评提交   -- 「滴答」双音下行（1.0k + 0.8k，170ms）
  mode.wav 模式切换   -- 「滴--」短低 + 长高（0.6k + 1.2k，235ms）
  err.wav  边界拒绝   -- 低频「嘟-」（280Hz 软方波，230ms）

用法：
  python3 tools/gen_ui_sounds.py [--out DIR]     # 默认 tools/ui_sounds/
  # 然后把 4 个 wav 拷贝到 SD 卡 /sdcard/audio/ui/

仅用标准库（wave/math/struct），无第三方依赖。
所有样本首尾 3ms 线性淡入淡出防爆音；幅度 0.55 满刻度。
"""
import math
import struct
import sys
import wave
from pathlib import Path

SR = 16000                 # 采样率（与 audio_player WAV 直播路径一致）
AMP = 0.55                 # 满刻度幅度（避免大音量失真）
EDGE_MS = 3                # 淡入淡出边沿


def tone(freq_hz, dur_ms, decay=0.0, duty=None):
    """单音采样：正弦（duty=None）或软方波（正弦削顶，保留谐波更醒耳）。

    decay > 0 时叠加指数衰减包络 exp(-decay*t)；首尾 EDGE_MS 线性淡变。
    """
    n = int(SR * dur_ms / 1000)
    edge = int(EDGE_MS * SR / 1000)
    out = []
    for i in range(n):
        t = i / SR
        s = math.sin(2 * math.pi * freq_hz * t)
        if duty is not None and 0 < duty < 1:
            s = max(-1.0, min(1.0, s / duty))
        env = math.exp(-decay * t) if decay > 0 else 1.0
        lin = 1.0
        if i < edge:
            lin = (i + 1) / edge
        elif i > n - edge:
            lin = max(0.0, (n - i) / edge)
        out.append(env * s * lin)
    return out


def silence(dur_ms):
    return [0.0] * int(SR * dur_ms / 1000)


def write_wav(path, samples):
    frames = b"".join(
        struct.pack("<h", int(max(-32768, min(32767, round(s * AMP * 32767)))))
        for s in samples)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(frames)
    ms = len(samples) * 1000 // SR
    print(f"  {path.name:10s} {ms:3d}ms  {path.stat().st_size}B")


def main():
    argv = sys.argv[1:]
    out = Path(argv[argv.index("--out") + 1]) if "--out" in argv \
        else Path(__file__).resolve().parent / "ui_sounds"
    out.mkdir(parents=True, exist_ok=True)
    print(f"ui sounds -> {out} ({SR}Hz mono PCM16, <0.3s)")

    write_wav(out / "key.wav", tone(1800, 70, decay=18))
    write_wav(out / "rate.wav",
              tone(1000, 55, decay=10) + silence(35) + tone(780, 80, decay=8))
    write_wav(out / "mode.wav",
              tone(620, 45, decay=4) + silence(25) + tone(1240, 165, decay=2))
    write_wav(out / "err.wav", tone(280, 230, decay=1.5, duty=0.5))

    print("copy to SD: /sdcard/audio/ui/")


if __name__ == "__main__":
    main()
