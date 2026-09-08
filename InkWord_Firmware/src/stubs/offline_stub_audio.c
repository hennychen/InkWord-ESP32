/**
 * @file offline_stub_audio.c
 * @brief AUDIO 轴桩（INKWORD_FEATURE_AUDIO=0，开源通用化 Phase 1）
 *
 * 顶替 audio_player.c / es8311.c / mic_recorder.c / ui_sfx.c 四个
 * 编译单元（offline env build_src_filter 排除真实现 +<stubs/> 纳入
 * 本桩）。头文件原样保留——include 既有头保证签名与真实现严格一致
 * （签名漂移编译期即报错，协议常量双侧同步纪律）。
 *
 * 桩语义（返回值走既有降级路径，业务文件零改动）：
 *   - audio_play_file / audio_play_file_sync 返回非 0 → 发音失败分支
 *     （study_mode_handle_action(3) 现有短震降级，PRD 口径不变）；
 *   - mic_recorder_record / mic_recorder_upload 返回非 0 → 跟读评测
 *     任务现有失败收尾；
 *   - audio_init / es8311_init 返回 0（setup 不检查，静默通过）；
 *   - sync 类查询恒失败态（audio_is_playing=false 等）。
 *
 * 纪律：被顶替模块新增公开 API 时必须同步本桩（链接错误兜底提醒）。
 */
#include "audio_player.h"
#include "es8311.h"
#include "mic_recorder.h"
#include "ui_sfx.h"

#include <stdint.h>
#include <stddef.h>

/* ---- audio_player.h ---- */

int audio_init(void)                       { return 0; }   /* STUB */
void audio_deinit(void)                    { }             /* STUB */
int audio_set_sample_rate(uint32_t rate)   { (void)rate; return 0; }   /* STUB */
int audio_play_file(const char *path)      { (void)path; return -1; }  /* STUB：走发音失败降级 */
int audio_play_file_sync(const char *path) { (void)path; return -1; }  /* STUB */
int audio_play_test_tone(void)             { return -1; }  /* STUB */
void audio_stop(void)                      { }             /* STUB */
bool audio_is_playing(void)                { return false; }   /* STUB */
void audio_suspend(bool on)                { (void)on; }   /* STUB */
int audio_bus_reconfigure(void)            { return 0; }   /* STUB */

/* ---- es8311.h ---- */

int es8311_probe(void)                     { return -1; }  /* STUB：无 codec 在位 */
int es8311_init(void)                      { return 0; }   /* STUB */
void es8311_deinit(void)                   { }             /* STUB */
int es8311_set_sample_rate(uint32_t rate)  { (void)rate; return 0; }   /* STUB */
void es8311_dac_start(void)                { }             /* STUB */
void es8311_dac_stop(void)                 { }             /* STUB */
int es8311_adc_start(int gain_lvl)         { (void)gain_lvl; return -1; }  /* STUB */
void es8311_adc_stop(void)                 { }             /* STUB */
int es8311_set_volume(int vol_0_100)       { (void)vol_0_100; return 0; }  /* STUB：NVS 音量镜像无处同步，静默 */
bool es8311_present(void)                  { return false; }  /* STUB：无 codec 在位 */

/* ---- mic_recorder.h ---- */

int mic_recorder_record(uint8_t **out_wav, size_t *out_len,
                        volatile bool *cancel, volatile bool *send_now,
                        int max_ms)                        /* STUB：录音不可用 */
{
    (void)out_wav; (void)out_len; (void)cancel; (void)send_now; (void)max_ms;
    return -1;
}

int mic_recorder_upload(const char *cloud_id, const uint8_t *wav,
                        size_t len, pron_result_t *out)    /* STUB：上传不可用 */
{
    (void)cloud_id; (void)wav; (void)len; (void)out;
    return -1;
}

/* ---- ui_sfx.h ---- */

void ui_sfx_init(void)                     { }             /* STUB：无提示音资产探测 */
void ui_sfx_play(ui_sfx_t ev)              { (void)ev; }   /* STUB：静默降级（真实现缺样本同款行为） */
