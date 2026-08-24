/**
 * @file mic_recorder.h
 * @brief INMP441 麦克风录音 + 发音评测上传 (P1，2026-08-24)
 *
 * AI 后端化红线：设备只采集/上传/消费评分。录音期重配 I2S0 为
 * 16kHz 全双工（SCK/WS 与 MAX98357 共线，WIRING §2.7），录完恢复
 * 播放配置；采样缓冲 PSRAM（≤96KB）录完即释，不动 WordEntry。
 *
 * 协议冻结见 docs/AI_SPEECH_ASSESSMENT.md §2：
 * POST /api/device/pronunciation?wordId={Guid}，multipart 字段名 "file"，
 * WAV 16kHz/16bit/mono；响应 data.total ≥60 视为通过。P2B 对话复用
 * 录音本体（max_ms 放宽 10s/320KB PSRAM），上传走 chat_mode 自备。
 */
#ifndef INKWORD_MIC_RECORDER_H
#define INKWORD_MIC_RECORDER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 评测结果（响应 data 字段子集；phonemes 音素明细设备端不消费） */
typedef struct {
    int  total;          /**< 0~100 总分；≥60 通过 */
    int  duration_ms;    /**< 话音时长（服务端测得） */
    char engine[16];     /**< "heuristic"（基础评分）/ "gop"（精细评分） */
} pron_result_t;

/**
 * @brief 录一段音频，返回完整 WAV（44B 头 + PCM，PSRAM 分配）。
 *
 * 内部完成 I2S 总线让渡（audio_suspend → 重配全双工 → 录 → 恢复
 * audio_bus_reconfigure → audio_suspend(false)）。调用前无需先播完
 * 音频——本函数会打断在播曲目（让渡语义）。
 * VAD 提前断：尾端静音 ≥800ms（块能量 < -35dBFS）即收；从未检测
 * 到话音时返回 -3（调用方提示「未听到」免无意义上传）。
 *
 * @param out_wav   输出 WAV 缓冲指针（调用方 free()；失败为 NULL）
 * @param out_len   输出 WAV 总字节数（含 44B 头）
 * @param cancel    可空：外部取消标志，录音循环逐块检查（-2 丢弃）
 * @param send_now  可空：手动断标志，提前收尾保留已录内容（P2B
 *                  对话「说完即发」；从未有话音仍 -3）
 * @param max_ms    录音上限毫秒（1000~10000 钳位；P1 跟读 3000、
 *                  P2B 对话 10000 → 320044B PSRAM 峰值纪律内）
 * @return 0 成功；-1 I2S/内存失败；-2 用户取消；-3 无话音
 */
int mic_recorder_record(uint8_t **out_wav, size_t *out_len,
                        volatile bool *cancel, volatile bool *send_now,
                        int max_ms);

/**
 * @brief 上传 WAV 评分（流式分段写，不整包拼装——PSRAM 峰值纪律）。
 * @return 0 成功（out 填充）；-1 网络/HTTP/解析失败
 */
int mic_recorder_upload(const char *cloud_id, const uint8_t *wav,
                        size_t len, pron_result_t *out);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_MIC_RECORDER_H */
