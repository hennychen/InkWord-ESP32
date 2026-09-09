/**
 * @file mic_recorder.c
 * @brief ES8311 ADC 录音与发音评测上传实现 (P1；ES8311 接入 2026-08-24)
 *
 * 录音：I2S0 重配 16kHz/32bit 槽/mono 全双工；ES8311 ADC 输出 16bit
 * 数据居 32bit 槽高位，软件 >>16 截取（与 INMP441 旧驱动同策略）；
 * 全双工纪律：每读一批向 TX 写等量静音零样本（防功放杂音/DMA 异常，
 * AI_SPEECH_ASSESSMENT §3.2）。
 * VAD：块（16ms）能量 dBFS，尾端连续静音 ≥800ms 提前断（起始 600ms
 * 保护不判）；全程无话音返回 -3 免无意义上传。
 * ⚠️ VAD 阈值 -35dBFS 原为 INMP441 标定，ES8311 模拟麦+PGA 增益
 * 噪声底不同，上机实测后需复标（TODO: VAD_CALIB）。
 *
 * 上传：esp_http_client open/write 流式分段（multipart 头/体/尾三段
 * 写出，不整包拼装——词池满载时 PSRAM 余量紧，峰值纪律）；WAV 协议
 * 硬约束 16kHz/16bit/mono，时长上限由 max_ms 参数控制（1000~10000 钳位，
 * P1 跟读 3s / P2B 对话 10s=320044B，均在本模块录音参数内天然保证）。
 */
#include "mic_recorder.h"
#include "debug_log.h"
#include "gpio_config.h"
#include "audio_player.h"   /* audio_suspend / audio_bus_reconfigure */
#include "es8311.h"         /* ES8311 codec ADC 控制（2026-08-24） */
#include "sync_client.h"    /* base_url / device_key 配置源 */

#include "driver/i2s.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "esp_heap_caps.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "MIC";

#define MIC_I2S_PORT        I2S_NUM_0     /* 与 audio_player 共享端口 */
#define MIC_SAMPLE_RATE     16000
#define MIC_MIN_MS          1000          /* max_ms 钳位下限 */
#define MIC_MAX_MS          10000         /* 钳位上限（320KB PSRAM 纪律） */
#define MIC_CHUNK_SAMPLES   (512)         /* 单块样本（16bit → DMA 1KB） */
#define MIC_I2S_TIMEOUT_MS  (200)

/* VAD（块粒度：256 样本 = 16ms） */
/* TODO: VAD_CALIB — -35dBFS 原为 INMP441 标定，ES8311 模拟麦+PGA 噪声底不同，
 * 上机实测后需复标（PGA gain=3 时建议 -30~-40 范围扫描） */
#define VAD_SILENCE_DB      (-35.0f)      /* 块能量阈值（dBFS） */
#define VAD_TAIL_MS         (800)         /* 尾端连续静音提前断 */
#define VAD_START_MS        (600)         /* 起始保护（按键/环境噪声） */

/* 单次采集静态缓冲（单任务串行使用；免占任务栈）
 * 2026-09-09 P0-1：32bit→16bit 槽。16bit 帧 BCLK=32fs，SCLK 作 mclk 源
 * ×8 后恰为 256fs（与播放同构）；32bit 槽下 ×8 会倍频至 512fs，
 * codec 内部分频失配 → SDOUT 速率错 2 倍 */
static int16_t s_raw[MIC_CHUNK_SAMPLES];
static int16_t s_zeros[MIC_CHUNK_SAMPLES];

/* ---- 44B 标准 WAV 头（RIFF/PCM 16bit/mono/16kHz） ---- */
typedef struct __attribute__((packed)) {
    char     riff[4];         /* "RIFF" */
    uint32_t riff_size;
    char     wave[4];         /* "WAVE" */
    char     fmt[4];          /* "fmt " */
    uint32_t fmt_size;        /* 16 */
    uint16_t format;          /* 1 = PCM */
    uint16_t channels;        /* 1 */
    uint32_t sample_rate;     /* 16000 */
    uint32_t byte_rate;       /* rate*ch*bits/8 */
    uint16_t block_align;     /* ch*bits/8 */
    uint16_t bits;            /* 16 */
    char     data[4];         /* "data" */
    uint32_t data_size;
} wav44_t;

static void wav44_build(uint8_t *buf, uint32_t data_bytes)
{
    wav44_t *h = (wav44_t *)buf;
    memcpy(h->riff, "RIFF", 4);
    h->riff_size = 36 + data_bytes;
    memcpy(h->wave, "WAVE", 4);
    memcpy(h->fmt, "fmt ", 4);
    h->fmt_size = 16;
    h->format = 1;
    h->channels = 1;
    h->sample_rate = MIC_SAMPLE_RATE;
    h->byte_rate = MIC_SAMPLE_RATE * 2;
    h->block_align = 2;
    h->bits = 16;
    memcpy(h->data, "data", 4);
    h->data_size = data_bytes;
}

/* 重配 I2S0 全双工（调用前置 audio_suspend(true) 打断播放任务）
 * 16bit/16k：与播放制式同构，ES8311 ADC SDP 16bit（REG0A=0x0C）对位 */
static int i2s_install_duplex(void)
{
    /* audio_player 的 TX-only 驱动可能仍安装（suspend 不卸载），先卸 */
    i2s_driver_uninstall(MIC_I2S_PORT);

    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
        .sample_rate = MIC_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  /* L/R=GND 固定左 */
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = MIC_CHUNK_SAMPLES,
        .use_apll = false,
        .tx_desc_auto_clear = true,
    };
    if (i2s_driver_install(MIC_I2S_PORT, &cfg, 0, NULL) != ESP_OK)
        return -1;

    i2s_pin_config_t pins = {
        .bck_io_num   = I2S_BCK_PIN,
        .ws_io_num    = I2S_WS_PIN,
        .data_out_num = I2S_DATA_OUT_PIN,   /* 全双工仍占用 TX（写静音） */
        .data_in_num  = I2S_DATA_IN_PIN,
    };
    if (i2s_set_pin(MIC_I2S_PORT, &pins) != ESP_OK)
        return -1;
    return 0;
}

/**
 * 采集 PCM 到 pcm 缓冲。
 * @return 0 录满/提前断（正常）；-1 I2S 失败；-2 取消；-3 无话音
 * @param out_peak_db 全程块能量峰值 dBFS（可 NULL）——mic 可用性诊断：
 *         峰值 ≤-60dB 量级=采集链路死（哑麦/未供偏），说话但 -35 附近
 *         =VAD 阈值失配需复标（TODO: VAD_CALIB 的实测依据）
 */
static int record_pcm(int16_t *pcm, volatile bool *cancel,
                      volatile bool *send_now, int *out_samples,
                      int max_samples, double *out_peak_db)
{
    int total = 0, silent_ms = 0;
    bool ever_voiced = false;
    double peak_db = -99.0;
    if (out_peak_db) *out_peak_db = -99.0;
    /* bring-up 诊断（P0-1）：非零计数+首块峰值定位悬空 DIN vs ADC 无输出 */
    int dbg_blk = 0, dbg_nz = 0;
    int32_t dbg_hi = 0;

    while (total < max_samples) {
        if (cancel && *cancel) return -2;
        if (send_now && *send_now) {          /* 手动断：说完即发 */
            *out_samples = total;
            if (out_peak_db) *out_peak_db = peak_db;
            return ever_voiced ? 0 : -3;
        }

        size_t br = 0;
        if (i2s_read(MIC_I2S_PORT, s_raw, sizeof(s_raw), &br,
                     pdMS_TO_TICKS(MIC_I2S_TIMEOUT_MS)) != ESP_OK || br == 0)
            return -1;
        int n = (int)(br / sizeof(int16_t));

        /* 全双工纪律：读多少样本向 TX 写多少静音零（16bit 槽等量） */
        size_t bw = 0;
        i2s_write(MIC_I2S_PORT, s_zeros, (size_t)n * sizeof(int16_t),
                  &bw, pdMS_TO_TICKS(MIC_I2S_TIMEOUT_MS));

        if (total + n > max_samples) n = max_samples - total;
        int base = total;
        uint64_t acc = 0;
        for (int i = 0; i < n; i++) {
            int16_t v = s_raw[i];
            if (dbg_blk < 3) {
                int16_t a = v < 0 ? -v : v;
                if (a > dbg_hi) dbg_hi = a;
                if (i < 4)
                    LOG_I("MICDBG blk%d [%d]=0x%04X (%d)",
                          dbg_blk, i, (uint16_t)v, v);
            }
            if (v != 0) dbg_nz++;
            pcm[base + i] = v;
            acc += (uint64_t)((int64_t)v * v);
        }
        dbg_blk++;
        total += n;

        /* VAD：块 RMS 能量（dBFS） */
        int chunk_ms = n * 1000 / MIC_SAMPLE_RATE;
        int elapsed_ms = total * 1000 / MIC_SAMPLE_RATE;
        double rms = sqrt((double)acc / (double)n);
        double db = 20.0 * log10(rms / 32768.0 + 1e-9);
        if (db > peak_db) peak_db = db;

        if (db >= VAD_SILENCE_DB) {
            ever_voiced = true;
            silent_ms = 0;
        } else if (elapsed_ms >= VAD_START_MS) {
            silent_ms += chunk_ms;
            if (silent_ms >= VAD_TAIL_MS) {
                *out_samples = total;
                if (out_peak_db) *out_peak_db = peak_db;
                LOG_W("MICDBG: nz=%d/%d peak=%ld",
                      dbg_nz, total, (long)dbg_hi);
                return ever_voiced ? 0 : -3;
            }
        }
    }
    *out_samples = total;
    if (out_peak_db) *out_peak_db = peak_db;
    LOG_W("MICDBG: nz=%d/%d peak=%ld",
          dbg_nz, total, (long)dbg_hi);
    return ever_voiced ? 0 : -3;
}

int mic_recorder_record(uint8_t **out_wav, size_t *out_len,
                        volatile bool *cancel, volatile bool *send_now,
                        int max_ms)
{
    if (!out_wav || !out_len) return -1;
    if (max_ms < MIC_MIN_MS) max_ms = MIC_MIN_MS;
    if (max_ms > MIC_MAX_MS) max_ms = MIC_MAX_MS;
    int max_samples = MIC_SAMPLE_RATE * max_ms / 1000;
    size_t buf_total = 44 + (size_t)max_samples * 2;
    *out_wav = NULL;
    *out_len = 0;

    /* 总线让渡：打断在播曲目并等待音频任务退出 */
    audio_suspend(true);

    uint8_t *buf = (uint8_t *)heap_caps_malloc(buf_total,
                                               MALLOC_CAP_SPIRAM);
    if (!buf) {
        LOG_E("PSRAM alloc %u failed", (unsigned)buf_total);
        audio_bus_reconfigure();
        audio_suspend(false);
        return -1;
    }

    int rc = i2s_install_duplex();
    if (rc == 0) {
        /* codec 分频切 16k（SCLK 源下 16bit 帧×8=256fs 与播放同构）；
         * 播放侧 44.1k 系数由 audio_bus_reconfigure 结束后恢复 */
        es8311_set_sample_rate(MIC_SAMPLE_RATE);
        /* ES8311 ADC 起录：PGA 增益档 3（18dB，板载麦/外接麦均适用） */
        es8311_adc_start(3);
        int samples = 0;
        double peak_db = -99.0;
        rc = record_pcm((int16_t *)(buf + 44), cancel, send_now,
                        &samples, max_samples, &peak_db);
        es8311_adc_stop();
        i2s_driver_uninstall(MIC_I2S_PORT);
        if (rc == 0) {
            wav44_build(buf, (uint32_t)samples * 2);
            *out_wav = buf;
            *out_len = 44 + (size_t)samples * 2;
            LOG_I("recorded %d samples (%d ms) peak=%.0f dBFS",
                  samples, samples * 1000 / MIC_SAMPLE_RATE, peak_db);
        } else if (rc == -3) {
            /* 无话音诊断：峰值 vs 阈值对比直接区分哑麦/阈值失配 */
            LOG_W("no voice: peak=%.0f dBFS vs VAD %.0f (mic dead or "
                  "threshold miscalib)", peak_db, VAD_SILENCE_DB);
        }
    }

    /* 恢复播放侧配置（uninstall 后 audio_player 重装 44.1k TX-only；
     * 其内部对未安装驱动的 uninstall 调用幂等） */
    audio_bus_reconfigure();
    audio_suspend(false);

    if (rc != 0) free(buf);
    return rc;
}

/* 按 URL 前缀选传输：http: 明文 TCP，其余 TLS + 证书包
 * （sync_client/ota_manager/audio_sync 同策略） */
static void mic_fill_cfg(esp_http_client_config_t *cfg, const char *url,
                         int timeout_ms)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->url = url;
    cfg->timeout_ms = timeout_ms;
    if (strncmp(url, "http:", 5) == 0) {
        cfg->transport_type = HTTP_TRANSPORT_OVER_TCP;
    } else {
        cfg->transport_type = HTTP_TRANSPORT_OVER_SSL;
        cfg->crt_bundle_attach = arduino_esp_crt_bundle_attach;
    }
}

int mic_recorder_upload(const char *cloud_id, const uint8_t *wav,
                        size_t len, pron_result_t *out)
{
    static const char BND[] = "InkWordPron1886";
    static char resp[1024];

    char head[192], tail[48], url[192];
    int hl = snprintf(head, sizeof(head),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"pron.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n", BND);
    int fl = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", BND);
    snprintf(url, sizeof(url), "%s/api/device/pronunciation?wordId=%s",
             sync_get_base_url(), cloud_id);

    esp_http_client_config_t cfg;
    mic_fill_cfg(&cfg, url, 15000);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;

    char ctype[64];
    snprintf(ctype, sizeof(ctype),
             "multipart/form-data; boundary=%s", BND);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", ctype);
    esp_http_client_set_header(client, "X-Device-Key", sync_get_device_key());

    /* 流式分段写出（multipart 头/体/尾），免整包 PSRAM 拼装 */
    esp_err_t err = esp_http_client_open(client, hl + (int)len + fl);
    if (err == ESP_OK) {
        int w = esp_http_client_write(client, head, hl);
        if (w == hl) w = esp_http_client_write(client, (const char *)wav, (int)len);
        if (w == (int)len) w = esp_http_client_write(client, tail, fl);
        if (w != fl) err = ESP_FAIL;
    }

    int resp_len = 0;
    if (err == ESP_OK && esp_http_client_fetch_headers(client) >= 0)
        resp_len = esp_http_client_read(client, resp, sizeof(resp) - 1);

    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || resp_len <= 0) {
        LOG_W("pron upload failed: err=%d status=%d len=%d", err, status, resp_len);
        return -1;
    }
    resp[resp_len] = '\0';

    /* 信封 { code, message, data:{ total, durationMs, engine } } */
    int rc = -1;
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_Parse(resp);
    if (root) {
        cJSON *code = cJSON_GetObjectItem(root, "code");
        cJSON *data = cJSON_GetObjectItem(root, "data");
        cJSON *jtotal = data ? cJSON_GetObjectItem(data, "total") : NULL;
        if (cJSON_IsNumber(code) && code->valueint == 0 &&
            cJSON_IsNumber(jtotal)) {
            out->total = jtotal->valueint;
            cJSON *jdur = cJSON_GetObjectItem(data, "durationMs");
            cJSON *jeng = cJSON_GetObjectItem(data, "engine");
            if (cJSON_IsNumber(jdur)) out->duration_ms = jdur->valueint;
            if (cJSON_IsString(jeng) && jeng->valuestring)
                strncpy(out->engine, jeng->valuestring, sizeof(out->engine) - 1);
            rc = 0;
        }
        cJSON_Delete(root);
    }
    if (rc != 0) LOG_W("pron payload invalid");
    return rc;
}
