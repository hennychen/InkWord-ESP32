/**
 * @file audio_player.c
 * @brief I2S 音频驱动实现 (Task F-10)
 *
 * I2S 标准模式 -> MAX98357A。WAV(PCM) 直接流式播放；MP3 走 helix 解码回调。
 */
#include "audio_player.h"
#include "debug_log.h"
#include "gpio_config.h"

#include "driver/i2s.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>

/* 轻量 MP3 解码（libhelix-mp3，可选组件） */
#ifdef HAVE_LIBHELIX_MP3
#include "mp3dec.h"
#else
/* 无 MP3 解码库时，play_mp3 返回错误 */
#endif

static const char *TAG = "AUDIO";

#define I2S_PORT_NUM     I2S_NUM_0
#define I2S_READ_LEN     (1024)        /* 单次写 I2S 的帧数 */
#define MP3_BUF_LEN      (2048)        /* MP3 输入缓冲 */
#ifdef HAVE_LIBHELIX_MP3
#define PCM_BUF_LEN      (MAX_NGRAN * MAX_NCHAN * MAX_NSPC * 2) /* 单帧 PCM 字节数 */
#else
#define PCM_BUF_LEN      (4608)        /* MP3 最大 PCM 输出 (1152 * 2ch * 2bytes) */
#endif

static bool s_inited = false;
static bool s_i2s_installed = false;  /* 跟踪 I2S 驱动是否已安装 */
static volatile bool s_playing = false;
static volatile bool s_stop_req = false;

/* WAV 文件头（PCM, RIFF）最小解析 */
typedef struct __attribute__((packed)) {
    char     riff[4];        /* "RIFF" */
    uint32_t file_size;
    char     wave[4];        /* "WAVE" */
    char     fmt[4];         /* "fmt " */
    uint32_t fmt_size;
    uint16_t audio_format;   /* 1 = PCM */
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} wav_header_t;

static int i2s_configure_std(uint32_t sample_rate, uint16_t bits, uint16_t channels)
{
    /* 旧版 API: 重新安装驱动以切换采样率 */
    i2s_config_t i2s_cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = sample_rate,
        .bits_per_sample = (i2s_bits_per_sample_t)bits,
        .channel_format = (channels == 2) ? I2S_CHANNEL_FMT_RIGHT_LEFT
                                          : I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true,
    };
    /* 仅当驱动已安装时才卸载，避免首次调用报错 */
    if (s_i2s_installed) {
        i2s_driver_uninstall(I2S_PORT_NUM);
    }
    esp_err_t ret = i2s_driver_install(I2S_PORT_NUM, &i2s_cfg, 0, NULL);
    if (ret != ESP_OK) return -1;
    s_i2s_installed = true;

    i2s_pin_config_t pin_cfg = {
        .bck_io_num   = I2S_BCK_PIN,
        .ws_io_num    = I2S_WS_PIN,
        .data_out_num = I2S_DATA_OUT_PIN,
        .data_in_num  = I2S_PIN_NO_CHANGE,
    };
    ret = i2s_set_pin(I2S_PORT_NUM, &pin_cfg);
    return (ret == ESP_OK) ? 0 : -1;
}

int audio_init(void)
{
    if (s_inited) {
        return 0;
    }

    if (i2s_configure_std(I2S_SAMPLE_RATE, I2S_SAMPLE_BITS, 1) != 0) {
        LOG_E("i2s config failed");
        return -1;
    }

    s_inited = true;
    LOG_I("I2S audio initialized @ %dHz/%dbit mono -> MAX98357A", I2S_SAMPLE_RATE, I2S_SAMPLE_BITS);
    return 0;
}

void audio_deinit(void)
{
    if (!s_inited) return;
    audio_stop();
    i2s_stop(I2S_PORT_NUM);
    i2s_driver_uninstall(I2S_PORT_NUM);
    s_inited = false;
}

int audio_set_sample_rate(uint32_t sample_rate)
{
    if (!s_inited) return -1;
    int r = i2s_configure_std(sample_rate, I2S_SAMPLE_BITS, 1);
    LOG_I("sample rate -> %lu", (unsigned long)sample_rate);
    return r;
}

static void i2s_write_mono(const uint8_t *data, size_t len)
{
    size_t written = 0;
    i2s_write(I2S_PORT_NUM, data, len, &written, portMAX_DELAY);
}

/* ---- WAV(PCM) 播放 ---- */
static int play_wav(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG_E("open wav failed: %s", path);
        return -1;
    }

    wav_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 ||
        memcmp(hdr.riff, "RIFF", 4) != 0 || memcmp(hdr.wave, "WAVE", 4) != 0) {
        LOG_E("invalid wav header");
        fclose(f);
        return -1;
    }
    if (hdr.audio_format != 1) {
        LOG_E("only PCM wav supported (format=%d)", hdr.audio_format);
        fclose(f);
        return -1;
    }

    /* 按文件采样率重配 I2S */
    audio_set_sample_rate(hdr.sample_rate);

    uint8_t buf[I2S_READ_LEN];
    size_t rbytes;
    while (!s_stop_req) {
        rbytes = fread(buf, 1, sizeof(buf), f);
        if (rbytes == 0) break;
        i2s_write_mono(buf, rbytes);
    }
    fclose(f);
    return 0;
}

/* ---- MP3 播放（libhelix-mp3，可选） ---- */
static int play_mp3(const char *path)
{
#ifndef HAVE_LIBHELIX_MP3
    LOG_E("MP3 decoding not available (compile with HAVE_LIBHELIX_MP3)");
    (void)path;
    return -1;
#else
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG_E("open mp3 failed: %s", path);
        return -1;
    }

    HMP3Decoder decoder = MP3InitDecoder();
    if (!decoder) {
        LOG_E("MP3InitDecoder failed");
        fclose(f);
        return -1;
    }

    uint8_t *in_buf  = malloc(MP3_BUF_LEN);
    int16_t *pcm_buf = malloc(PCM_BUF_LEN);
    if (!in_buf || !pcm_buf) {
        LOG_E("mp3 alloc failed");
        free(in_buf); free(pcm_buf); MP3FreeDecoder(decoder); fclose(f);
        return -1;
    }

    size_t read_bytes = fread(in_buf, 1, MP3_BUF_LEN, f);
    size_t buf_fill = read_bytes;
    uint8_t *read_ptr = in_buf;
    int prev_sample_rate = 0;

    while (!s_stop_req) {
        int offset = MP3FindSyncWord(read_ptr, (int)buf_fill, &read_ptr);
        if (offset < 0) {
            memmove(in_buf, read_ptr, buf_fill);
            read_bytes = fread(in_buf + buf_fill, 1, MP3_BUF_LEN - buf_fill, f);
            if (read_bytes == 0) break;
            buf_fill += read_bytes;
            read_ptr = in_buf;
            continue;
        }
        buf_fill -= (read_ptr - in_buf);
        memmove(in_buf, read_ptr, buf_fill);

        MP3FrameInfo info;
        int err = MP3Decode(decoder, &read_ptr, (int *)&buf_fill, pcm_buf, 0);
        if (err) {
            LOG_D("MP3Decode err=%d, skip", err);
            if (buf_fill > 0) { read_ptr++; buf_fill--; }
            continue;
        }

        MP3GetLastFrameInfo(decoder, &info);
        if ((int)info.samprate != prev_sample_rate) {
            audio_set_sample_rate(info.samprate);
            prev_sample_rate = (int)info.samprate;
        }

        size_t pcm_bytes = (size_t)info.outputSamps * sizeof(int16_t);
        i2s_write_mono((uint8_t *)pcm_buf, pcm_bytes);

        memmove(in_buf, read_ptr, buf_fill);
        read_ptr = in_buf;

        if (buf_fill < MP3_BUF_LEN / 2) {
            read_bytes = fread(in_buf + buf_fill, 1, MP3_BUF_LEN - buf_fill, f);
            buf_fill += read_bytes;
            read_ptr = in_buf;
        }
    }

    free(in_buf); free(pcm_buf); MP3FreeDecoder(decoder);
    fclose(f);
    return 0;
#endif
}

static bool ends_with(const char *s, const char *suf)
{
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && strcasecmp(s + ls - lf, suf) == 0;
}

int audio_play_file(const char *path)
{
    if (!s_inited) audio_init();
    s_stop_req = false;
    s_playing = true;

    int ret;
    if (ends_with(path, ".wav")) {
        ret = play_wav(path);
    } else if (ends_with(path, ".mp3")) {
        ret = play_mp3(path);
    } else {
        LOG_E("unsupported format: %s", path);
        ret = -1;
    }

    s_playing = false;
    return ret;
}

void audio_stop(void)
{
    s_stop_req = true;
}

bool audio_is_playing(void)
{
    return s_playing;
}
