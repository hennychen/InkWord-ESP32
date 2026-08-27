/**
 * @file audio_player.c
 * @brief I2S 音频驱动实现 (Task F-10；P0A 异步化 2026-08-24；ES8311 接入 2026-08-24)
 *
 * I2S 标准模式 → ES8311 DAC → NS4150B 功放。WAV(PCM) 直接流式播放；
 * MP3 走 libhelix 解码。codec 寄存器经 I2C 配置（es8311.c/h）。
 *
 * P0A 异步化（替代原同步阻塞实现，音频播放异步化规范）：
 * - audio_play_file() 只投递路径队列（btn_scan 任务零阻塞）；
 * - 专用音频任务（8KB 栈，优先级 4 < btn_scan 5）消费队列播放；
 * - 代际计数 s_req_gen：每次提交自增，播放循环发现代际落后即退出，
 *   实现「重按打断重播」，无竞态窗口；
 * - audio_deinit() 唤醒并等待音频任务退出后再卸载 I2S，
 *   power_manager 深睡收口复用此语义（≤2s 阻塞，入睡路径可接受）。
 */
#include "audio_player.h"
#include "debug_log.h"
#include "gpio_config.h"
#include "es8311.h"    /* ES8311 codec I2C 驱动（2026-08-24） */

#include "driver/i2s.h"
#include "driver/gpio.h"
#include "soc/gpio_sig_map.h"   /* I2S0_MCLK_OUT_IDX（S3 sig 23） */
#include "rom/gpio.h"            /* gpio_matrix_out：MCLK 显式路由 */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <string.h>
#include <stdio.h>
#include <math.h>     /* 测试音 sinf（硬件链路验证） */

/* 轻量 MP3 解码（src/mp3/ 内嵌 libhelix，platformio.ini 全 env 定义宏；
 * 相对路径包含：arduino 递归编译不为兄弟目录加 -I */
#ifdef HAVE_LIBHELIX_MP3
#include "mp3/mp3dec.h"
#endif

static const char *TAG = "AUDIO";

#define I2S_PORT_NUM     I2S_NUM_0
#define I2S_READ_LEN     (1024)        /* 单次写 I2S 的帧数 */
#define MP3_BUF_LEN      (2048)        /* MP3 输入缓冲 */
#ifdef HAVE_LIBHELIX_MP3
/* 单帧 PCM 最大字节数 (2 granule * 2 ch * 576 samp * 2B) */
#define PCM_BUF_LEN      (MAX_NGRAN * MAX_NCHAN * MAX_NSAMP * 2)
#else
#define PCM_BUF_LEN      (4608)        /* MP3 最大 PCM 输出 (1152 * 2ch * 2bytes) */
#endif

/* ---- 异步播放基础设施（P0A） ---- */
#define AUDIO_PATH_MAX   128           /* 与 study_mode speak 路径缓冲对齐 */
#define AUDIO_QUEUE_LEN  4
#define AUDIO_TASK_STACK (8 * 1024)
#define AUDIO_TASK_PRIO  (4)           /* 低于 btn_scan(5)：播放中按键可响应 */
#define AUDIO_STOP_WAIT_MS (2000)      /* deinit/suspend 等任务退出上限 */

typedef struct {
    char     path[AUDIO_PATH_MAX];
    uint32_t gen;                      /* 代际：提交时快照，落后即被打断 */
} audio_msg_t;

static bool s_inited = false;
static bool s_i2s_installed = false;   /* 跟踪 I2S 驱动是否已安装 */
static volatile bool s_playing = false;
static volatile bool s_stop_req = false;
static volatile bool s_suspended = false; /* 录音让渡期拒绝播放（M5.2/P2B） */
static volatile bool s_task_alive = false;
static volatile bool s_shutdown = false;  /* deinit 序言，任务收尾快速退出 */
static volatile uint32_t s_req_gen = 0;   /* 每次提交自增的代际计数 */
static QueueHandle_t s_queue = NULL;
static TaskHandle_t  s_task = NULL;

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

/* 播放循环继续条件：未请求停止、非关停、代际仍最新（未被新提交打断） */
static bool play_keep_going(uint32_t gen)
{
    return !s_stop_req && !s_shutdown && s_req_gen == gen;
}

/* 忙等 s_playing 变 false（打断传播 + DMA 排空），超时上限 AUDIO_STOP_WAIT_MS */
static void wait_play_idle(void)
{
    int waited = 0;
    while (s_playing && waited < AUDIO_STOP_WAIT_MS) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
}

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
        /* mck_io_num 说明（2026-08-27 修正）：曾以 PCNT 三线 0Hz 推断
         * “NO_CHANGE 致不路由”，实为测量盲区——legacy 驱动对纯输出
         * 脚不开 input buffer，PCNT 读引脚输入侧恒为常值；i2s_write
         * 按音频实时阻塞（4s 探针 = 3+1s 音频时长）证明外设在发送。
         * 取 0 与零初始化等价：MCLK 输出到 GPIO0，未接线悬空无害。 */
#if defined(ES8311_MCLK_PIN) && ES8311_MCLK_PIN > 0
        .mck_io_num   = ES8311_MCLK_PIN,
#else
        .mck_io_num   = 0,           /* 仅驱动 MCLK 输出；无外部接线 */
#endif
        .bck_io_num   = I2S_BCK_PIN,
        .ws_io_num    = I2S_WS_PIN,
        .data_out_num = I2S_DATA_OUT_PIN,
        .data_in_num  = I2S_PIN_NO_CHANGE,
    };
    ret = i2s_set_pin(I2S_PORT_NUM, &pin_cfg);
    if (ret != ESP_OK) return -1;

    /* MCLK 显式 GPIO matrix 路由（双保险）：legacy 驱动对 mck_io_num=0
     * 的路由行为无源码可证，直接绑定 I2S0 MCLK 输出信号（幂等） */
#if defined(ES8311_MCLK_PIN) && ES8311_MCLK_PIN >= 0
    gpio_matrix_out(ES8311_MCLK_PIN, I2S0_MCLK_OUT_IDX, false, false);
#endif
    return 0;
}

static void i2s_write_mono(const uint8_t *data, size_t len)
{
    /* legacy I2S 驱动 mono(ONLY_LEFT) 时只发单槽 BCLK=16×fs，ES8311
     * SCLK 作 mclk 源最大 ×8 只到 128×fs < 256×fs → DAC 失锁无声
     * （2026-08-26 实测：×1 出杂音 ×8 无声，根因即此）。改为
     * stereo(RIGHT_LEFT) 双槽 BCLK=32×fs，mono 样本复制到 L/R 两槽
     * （MAX98357A 时代同款手法，听感等价）
     *
     * 必须分块循环写（2026-08-27 SD 卡读音实测）：曾截断到
     * I2S_READ_LEN/2=512 样本/次，而 mono 48k MP3 帧输出 1152 样本
     * → 每帧丢 55% 样本 = 播放过快 + 帧尾跳变杂音 */
    static int16_t s_stereo[I2S_READ_LEN];   /* 单写者：播放代际/停播门卫互斥 */
    const int16_t *src = (const int16_t *)data;
    size_t total = len / sizeof(int16_t);
    size_t pos = 0;
    while (pos < total) {
        size_t n = total - pos;
        if (n > I2S_READ_LEN / 2) n = I2S_READ_LEN / 2;
        for (size_t i = 0; i < n; i++) {
            s_stereo[i * 2]     = src[pos + i];            /* L */
            s_stereo[i * 2 + 1] = src[pos + i];           /* R */
        }
        size_t written = 0;
        i2s_write(I2S_PORT_NUM, s_stereo, n * 2 * sizeof(int16_t),
                  &written, portMAX_DELAY);
        pos += n;
    }
}

/* ---- WAV(PCM) 播放 ---- */
static int play_wav(const char *path, uint32_t gen)
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
    while (play_keep_going(gen)) {
        rbytes = fread(buf, 1, sizeof(buf), f);
        if (rbytes == 0) break;
        i2s_write_mono(buf, rbytes);
    }
    fclose(f);
    return 0;
}

/* ---- MP3 播放（libhelix，src/mp3/） ---- */
static int play_mp3(const char *path, uint32_t gen)
{
#ifndef HAVE_LIBHELIX_MP3
    LOG_E("MP3 decoding not available (compile with HAVE_LIBHELIX_MP3)");
    (void)path; (void)gen;
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
    int buf_fill = (int)read_bytes;          /* helix 约定 bytesLeft 为 int */
    uint8_t *read_ptr = in_buf;
    int prev_sample_rate = 0;

    while (play_keep_going(gen)) {
        /* 同步字搜索（ESP8266Audio 变体：返回偏移量，不回写指针） */
        int offset = MP3FindSyncWord(read_ptr, buf_fill);
        if (offset < 0) {
            /* 未找到：丢弃已扫描数据，从文件补充 */
            buf_fill = 0;
            read_ptr = in_buf;
            read_bytes = fread(in_buf, 1, MP3_BUF_LEN, f);
            if (read_bytes == 0) break;
            buf_fill = (int)read_bytes;
            continue;
        }
        read_ptr += offset;
        buf_fill -= offset;

        MP3FrameInfo info;
        int err = MP3Decode(decoder, &read_ptr, &buf_fill, pcm_buf, 0);
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

        /* 解码后 read_ptr 指向剩余数据：压缩回缓冲起点，不足半满则补读 */
        memmove(in_buf, read_ptr, (size_t)buf_fill);
        read_ptr = in_buf;

        if (buf_fill < MP3_BUF_LEN / 2) {
            read_bytes = fread(in_buf + buf_fill, 1,
                               MP3_BUF_LEN - (size_t)buf_fill, f);
            buf_fill += (int)read_bytes;
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

/* 按扩展名分发（返回播放结果码） */
static int play_by_ext(const char *path, uint32_t gen)
{
    es8311_dac_start();     /* 起播前上电解静音（idle 收口后恢复） */
    if (ends_with(path, ".wav")) return play_wav(path, gen);
    if (ends_with(path, ".mp3")) return play_mp3(path, gen);
    LOG_E("unsupported format: %s", path);
    return -1;
}

/* ---- 音频任务：消费路径队列（P0A 核心） ---- */
static void audio_task(void *arg)
{
    (void)arg;
    audio_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) continue;
        if (s_shutdown || msg.path[0] == '\0') break;  /* deinit 唤醒退出 */

        s_stop_req = false;   /* 新曲目清除旧打断请求 */
        s_playing = true;
        play_by_ext(msg.path, msg.gen);
        s_playing = false;
        es8311_dac_stop();   /* 曲目间隙收口：DAC 模拟下电，消空闲底噪 */
    }
    s_playing = false;
    s_task_alive = false;
    vTaskDelete(NULL);        /* 自删除；句柄由 deinit 置空 */
}

int audio_init(void)
{
    if (s_inited) {
        return 0;
    }

    /* 1. 装载 I2S 总线（stereo 双槽：BCLK=32×fs 供 ES8311 凑 256×fs
     * 内部时钟，见 i2s_write_mono 注释） */
    if (i2s_configure_std(I2S_SAMPLE_RATE, I2S_SAMPLE_BITS, 2) != 0) {
        LOG_E("i2s config failed");
        return -1;
    }

    /* 2. 初始化 codec（I2C + 寄存器序列；内部幂等） */
    if (es8311_init() != 0) {
        LOG_E("codec init failed (module not wired?)");
        /* 非致命：继续建任务，播放时再 probe（上机调试友好） */
    } else {
        es8311_dac_start();     /* codec 数据通路就绪（静音态，等播放解除 mute） */
    }

    /* 3. 建队列 + 音频任务 */
    s_queue = xQueueCreate(AUDIO_QUEUE_LEN, sizeof(audio_msg_t));
    if (!s_queue) {
        LOG_E("audio queue create failed");
        i2s_driver_uninstall(I2S_PORT_NUM);
        s_i2s_installed = false;
        return -1;
    }

    s_shutdown = false;
    if (xTaskCreate(audio_task, "audio_play", AUDIO_TASK_STACK, NULL,
                    AUDIO_TASK_PRIO, &s_task) != pdPASS) {
        LOG_E("audio task create failed");
        vQueueDelete(s_queue);
        s_queue = NULL;
        i2s_driver_uninstall(I2S_PORT_NUM);
        s_i2s_installed = false;
        return -1;
    }
    s_task_alive = true;

    s_inited = true;
    LOG_I("I2S audio initialized @ %dHz/%dbit stereo(L=R) -> ES8311+NS4150B (async)",
          I2S_SAMPLE_RATE, I2S_SAMPLE_BITS);
    return 0;
}

void audio_deinit(void)
{
    if (!s_inited) return;

    s_stop_req = true;
    s_shutdown = true;
    if (s_queue) xQueueReset(s_queue);
    if (s_task_alive) {
        audio_msg_t quit = { .path = { 0 }, .gen = 0 };
        xQueueSend(s_queue, &quit, 0);       /* 唤醒阻塞在收队的任务 */
        wait_play_idle();
        int waited = 0;
        while (s_task_alive && waited < AUDIO_STOP_WAIT_MS) {
            vTaskDelay(pdMS_TO_TICKS(10));
            waited += 10;
        }
        if (s_task_alive) {                  /* 兜底强删（极端阻塞情形） */
            vTaskDelete(s_task);
            s_task_alive = false;
        }
    }
    s_task = NULL;

    if (s_queue) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
    if (s_i2s_installed) {
        i2s_stop(I2S_PORT_NUM);
        i2s_driver_uninstall(I2S_PORT_NUM);
        s_i2s_installed = false;
    }
    /* codec 掉电（suspend 序列，保留 I2C 驱动） */
    es8311_dac_stop();
    es8311_deinit();
    s_shutdown = false;
    s_suspended = false;
    s_inited = false;
}

int audio_set_sample_rate(uint32_t sample_rate)
{
    if (!s_inited && !s_suspended) return -1;
    /* channels 必须 2（stereo L=R，与 init/resume 一致）：曾传 1 重装成
     * ONLY_LEFT 单槽帧，i2s_write_mono 仍按 stereo 复制 L/R 交错写入，
     * 帧解释错位 → 真机 MP3 播放杂音+慢放（2026-08-27 SD 卡读音实测；
     * 嵌入人声 WAV 不走此路径故未暴露）。mono 单槽坑见 2026-08-26 注释 */
    int r = i2s_configure_std(sample_rate, I2S_SAMPLE_BITS, 2);
    /* 同步 codec 时钟系数（256×fs；SCLK 作 mclk 源） */
    es8311_set_sample_rate(sample_rate);
    LOG_I("sample rate -> %lu", (unsigned long)sample_rate);
    return r;
}

int audio_play_file(const char *path)
{
    if (!path || !path[0]) return -1;
    if (s_suspended) {
        LOG_D("suspended (mic owns I2S), drop play: %s", path);
        return -1;
    }
    if (!s_inited && audio_init() != 0) return -1;

    audio_msg_t msg;
    strncpy(msg.path, path, AUDIO_PATH_MAX - 1);
    msg.path[AUDIO_PATH_MAX - 1] = '\0';
    msg.gen = ++s_req_gen;      /* 新提交即令旧代际播放循环退出（打断） */

    xQueueReset(s_queue);       /* 旧请求语义上已被打断，清队防串播 */
    xQueueSend(s_queue, &msg, 0);   /* reset 后必有空位，无需阻塞 */
    return 0;
}

int audio_play_file_sync(const char *path)
{
    if (!path || !path[0]) return -1;
    if (s_suspended) return -1;
    if (!s_inited && audio_init() != 0) return -1;

    audio_stop();               /* 打断在播曲目并清队 */
    wait_play_idle();           /* 等音频任务退出播放循环，避免双写 I2S */

    uint32_t gen = ++s_req_gen;
    s_stop_req = false;
    s_playing = true;
    int ret = play_by_ext(path, gen);
    s_playing = false;
    return ret;
}

/* ---- 嵌入人声 WAV（bring-up 正常音频判据，2026-08-27） ----
 * 生成链（macOS）：say -v Tingting -o aiff "音频链路测试。你好，墨词。"
 *   → afconvert -f WAVE -d LEI16@44100 -c 1（44.1k/16bit/mono，
 *   与固件 I2S 配置逐项匹配）；platformio.ini embed_files 嵌入。
 * 判据（人耳对语音敏感度远超蜂音）：能听清字句 = 链路通；
 * 机器人声/变调 = 时钟错档；含糊噪声 = 数据错位。 */
extern const uint8_t _binary_src_test_voice_wav_start[];
extern const uint8_t _binary_src_test_voice_wav_end[];

int audio_play_test_tone(void)
{
    if (s_suspended) return -1;             /* 录音让渡期不可测 */
    if (!s_inited && audio_init() != 0) return -1;

    audio_stop();                           /* 打断在播曲目（双写互斥） */
    wait_play_idle();

    /* 解析嵌入 WAV：RIFF 头 + 扫描 data chunk（fmt_size 可变，
     * 不假设 44 字节固定头） */
    const uint8_t *wav = _binary_src_test_voice_wav_start;
    size_t len = (size_t)(_binary_src_test_voice_wav_end -
                          _binary_src_test_voice_wav_start);
    if (len < sizeof(wav_header_t) + 8) {
        LOG_E("embedded voice too small (%u)", (unsigned)len);
        return -1;
    }
    const wav_header_t *h = (const wav_header_t *)wav;
    if (memcmp(h->riff, "RIFF", 4) != 0 || memcmp(h->wave, "WAVE", 4) != 0) {
        LOG_E("embedded voice: not RIFF/WAVE");
        return -1;
    }
    size_t off = 20 + h->fmt_size;          /* 跳过 fmt chunk */
    uint32_t pcm_len = 0;
    while (off + 8 <= len) {                /* 扫 data chunk */
        const uint8_t *id = wav + off;
        uint32_t sz;
        memcpy(&sz, wav + off + 4, 4);
        if (memcmp(id, "data", 4) == 0) {
            pcm_len = sz;
            off += 8;
            break;
        }
        off += 8 + sz + (sz & 1);           /* 奇数长 chunk 1 字节对齐 */
    }
    if (pcm_len == 0 || off + pcm_len > len) {
        LOG_E("embedded voice: data chunk not found");
        return -1;
    }
    LOG_I("embedded voice: %luHz %ubit %uch %.1fs",
          (unsigned long)h->sample_rate, h->bits_per_sample,
          h->num_channels,
          (double)pcm_len / h->byte_rate);

    uint32_t gen = ++s_req_gen;
    s_stop_req = false;
    es8311_set_sample_rate(h->sample_rate); /* 与 WAV 采样率同步 */
    es8311_dac_start();

    /* 单次播报（bring-up 已验收，循环版 2026-08-27 收尾移除） */
    s_playing = true;
    for (size_t done = 0;
         done < pcm_len && play_keep_going(gen); ) {
        size_t n = pcm_len - done;
        if (n > I2S_READ_LEN) n = I2S_READ_LEN;
        i2s_write_mono(wav + off + done, n);
        done += n;
    }
    s_playing = false;
    es8311_dac_stop();   /* 播毕模拟下电：消空闲嘶声 */
    LOG_I("voice self-test done");
    return 0;
}

/* ---- 小星星旋律循环已删（bring-up 2026-08-27 验收通过，
 * REG00=0x80 正常态 + MCLK 实线拓扑定型，诊断使命完成） ---- */

void audio_stop(void)
{
    s_stop_req = true;
    if (s_queue) xQueueReset(s_queue);
}

bool audio_is_playing(void)
{
    return s_playing;
}

void audio_suspend(bool on)
{
    if (on) {
        audio_stop();           /* 打断在播曲目 */
        wait_play_idle();       /* 等播放循环退出（DMA 排空）再交总线 */
        es8311_dac_stop();      /* codec DAC 静音（防录音期间杂音） */
        s_suspended = true;
        LOG_I("audio suspended (mic takes over I2S0)");
    } else {
        s_suspended = false;
        es8311_dac_start();     /* 恢复 codec DAC 通路 */
        LOG_I("audio resumed");
    }
}

int audio_bus_reconfigure(void)
{
    /* 录音占用 I2S0（全双工重配）后恢复播放侧单声道配置 */
    if (s_i2s_installed) {
        i2s_driver_uninstall(I2S_PORT_NUM);
        s_i2s_installed = false;
    }
    int r = i2s_configure_std(I2S_SAMPLE_RATE, I2S_SAMPLE_BITS, 2);
    /* 同步 codec 时钟系数（256×fs；SCLK 作 mclk 源） */
    es8311_set_sample_rate(I2S_SAMPLE_RATE);
    es8311_dac_start();             /* 录音后恢复 codec DAC 通路 */
    LOG_I("bus reconfigured for playback");
    return r;
}
