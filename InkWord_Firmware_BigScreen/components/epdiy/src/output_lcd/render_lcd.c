#include <stdint.h>
#include <string.h>

#include "../output_common/render_method.h"

#ifdef RENDER_METHOD_LCD

#include <rom/cache.h>
#include <esp_log.h>
#include <esp_rom_sys.h>

#include "render_lcd.h"
#include "epd_board.h"
#include "epdiy.h"
#include "../epd_internals.h"
#include "lcd_driver.h"
#include "../output_common/line_queue.h"
#include "../output_common/lut.h"
#include "../output_common/render_context.h"

static bool IRAM_ATTR fill_line_noop(RenderContext_t* ctx, uint8_t *line) {
    memset(line, 0x00, ctx->display_width / 4);
    return false;
}

static bool IRAM_ATTR fill_line_white(RenderContext_t* ctx, uint8_t *line) {
    memset(line, CLEAR_BYTE, ctx->display_width / 4);
    return false;
}

static bool IRAM_ATTR fill_line_black(RenderContext_t* ctx, uint8_t *line) {
    memset(line, DARK_BYTE, ctx->display_width / 4);
    return false;
}

__attribute__((optimize("O3")))
static bool IRAM_ATTR retrieve_line_isr(RenderContext_t* ctx, uint8_t *buf) {
    if (ctx->lines_consumed >= ctx->lines_total) {
        return false;
    }
    int thread = ctx->line_threads[ctx->lines_consumed];
    assert(thread < NUM_RENDER_THREADS);

    LineQueue_t *lq = &ctx->line_queues[thread];

    BaseType_t awoken = pdFALSE;

    if (lq_read(lq, buf) != 0) {
        // [InkWord 修复] 1920 宽产速不均时目标线程 lq 瞬时空：立即置
        // error 会造成确定性卡死（run34 实证 prepared=96/consumed=64）。
        // 先读其他线程 lq 垫行（错行优于死锁），连续 64 次全空才置
        // error；垫行用白线 0xFF，视觉干扰最小。
        static volatile int s_starve = 0;
        bool filled = false;
        for (int alt = 0; alt < NUM_RENDER_THREADS; alt++) {
            if (alt != thread && lq_read(&ctx->line_queues[alt], buf) == 0) {
                filled = true;
                break;
            }
        }
        if (!filled) {
            if (++s_starve > 64) {
                ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
            }
            memset(buf, 0xFF, ctx->display_width / 4);
        } else {
            s_starve = 0;
        }
    }

    if (ctx->lines_consumed >= ctx->display_height) {
        memset(buf, 0x00, ctx->display_width / 4);
    }
    // [run86] 批边界流残差补偿: 边界重启后引擎行窗从流内 Z 字节残差
    // 开始(屏显实证末批整体左移≈8px=1字节, 行尾撕裂缝落入右边框
    // 黑区不可见)。末批行整体前补 Z 字节黑垫吸收早锁存; 垫落左边框
    // 黑区/白底行 x<Z 黑点亦并入左边框, 不可见。run86 z=1 后仍左移
    // 8px → 残差为字级(16bit 总线 1 字=2B=16px), run87 z=2。
    if (ctx->lines_consumed >= 1000) {
        const int z = 2;
        int n = ctx->display_width / 4;
        memmove(buf + z, buf, n - z);
        memset(buf, 0x00, z);
    }
    ctx->lines_consumed += 1;
    return awoken;
}

/// start the next frame in the current update cycle
static void IRAM_ATTR handle_lcd_frame_done(RenderContext_t *ctx) {
    epd_lcd_frame_done_cb(NULL, NULL);
    epd_lcd_line_source_cb(NULL, NULL);

    // [run75 修复] 帧输出完毕：放行忙等中的 feed 线程（残留行丢弃）
    ctx->frame_output_done = 1;

    BaseType_t task_awoken = pdFALSE;
    xSemaphoreGiveFromISR(ctx->frame_done, &task_awoken);

    portYIELD_FROM_ISR();
}

void lcd_do_update(RenderContext_t *ctx) {

    epd_set_mode(1);

    // [InkWord 诊断] 帧参数一次性输出，供死锁现场比对
    ESP_LOGI("epd_lcd", "do_update: frames=%d lines_total=%d area=(%d,%d,%dx%d) drawn=%p",
             ctx->cycle_frames, ctx->lines_total, ctx->area.x, ctx->area.y,
             ctx->area.width, ctx->area.height, (void*)ctx->drawn_lines);

    for (uint8_t k = 0; k < ctx->cycle_frames; k++) {
        epd_lcd_frame_done_cb((frame_done_func_t)handle_lcd_frame_done, ctx);
        prepare_context_for_next_frame(ctx);

        // [run71 诊断] 每帧起点计数器基线：k 间对比可知上帧是否真跑完
        {
            uint32_t v0 = 0, e0 = 0, b0 = 0;
            epd_lcd_dbg_counters(&v0, &e0, &b0);
            ESP_LOGI("epd_lcd", "frame k=%d begin: vsync=%u eof=%u batches=%u",
                     k, (unsigned)v0, (unsigned)e0, (unsigned)b0);
        }

        // [run72 修复] 排空上帧残留行：LCD 每帧实际消费（eof×BOUNCE_BUF_LINES）
        // 少于 lines_total（1080 产 vs ~1000 消），尾差残留队列；不清则
        // 下帧起步空间 52 < 触发线 64，feed 满队列忙等、start_frame 永
        // 不触发（run71 快照实证 lq 全满 prepared=56 consumed=0）。
        // 残留为旧相位数据，本帧全量重算，丢弃安全；帧间 LCD 已 stop，
        // 无 EOF ISR 并发读。
        for (int q = 0; q < NUM_RENDER_THREADS; q++) {
            lq_reset(&ctx->line_queues[q]);
        }

        // [InkWord 修复] 排空残留计数：上一帧 ISR give 与主任务 take 的
        // 竞态可能遗留 1 个计数，不清会让本帧被假放行（瞬完）。
        while (xSemaphoreTake(ctx->frame_done, 0) == pdTRUE) {}

        // start both feeder tasks
        // [InkWord run42 修复] 原“对核二连发”（feed_tasks[!core] +
        // feed_tasks[core]）是 NUM_RENDER_THREADS=2 时代的写法，扩容到 4
        // 后线程 2/3 永远收不到 notify，阻塞在 ulTaskNotifyTake、不给
        // feed_done，每帧必超时（真机实证 feed_done[2] 超时而
        // consumed=1080 完整）。改为全量分发，保留先异核后本核的起跑顺序。
        for (int i = 0; i < NUM_RENDER_THREADS; i++) {
            if ((i % 2) != (int)xPortGetCoreID()) {
                xTaskNotifyGive(ctx->feed_tasks[i]);
            }
        }
        for (int i = 0; i < NUM_RENDER_THREADS; i++) {
            if ((i % 2) == (int)xPortGetCoreID()) {
                xTaskNotifyGive(ctx->feed_tasks[i]);
            }
        }

        // [InkWord 修复] 起跑预热：1920 宽屏 LUT 生产慢于消费（上游为
        // 960 宽屏设计，假设 feed 永远领先）。notify 后给 feed 短暂
        // 预热窗口填满 lq，避免 start_frame 预填+EOF 双填的起跑 burst
        // 瞬间抽干队列触发空队列防御（run34 实证 prepared=96 时
        // consumed=64 追尾置 error）。
        vTaskDelay(pdMS_TO_TICKS(3));

        // [InkWord 诊断] 有限等待替代 portMAX_DELAY：死锁时自动吐出
        // 管线内部状态而非永久阻塞（bring-up 排障用，稳定后可回退）
        BaseType_t got = xSemaphoreTake(ctx->frame_done, pdMS_TO_TICKS(25000));
        if (got != pdTRUE) {
            // [run71 诊断] 死锁现场全量快照：ISR 计数器 + 每线程队列水位
            uint32_t vsync = 0, eof = 0, batches = 0;
            epd_lcd_dbg_counters(&vsync, &eof, &batches);
            ESP_LOGE("epd_lcd",
                     "frame_done 超时！k=%d prepared=%d consumed=%d err=0x%X "
                     "vsync=%u eof=%u batches=%u",
                     k, atomic_load(&ctx->lines_prepared),
                     atomic_load(&ctx->lines_consumed), ctx->error,
                     (unsigned)vsync, (unsigned)eof, (unsigned)batches);
            for (int q = 0; q < NUM_RENDER_THREADS; q++) {
                LineQueue_t* lq = &ctx->line_queues[q];
                int cur = atomic_load(&lq->current);
                int lst = atomic_load(&lq->last);
                ESP_LOGE("epd_lcd", "  lq[%d] fill=%d/%d (cur=%d last=%d)",
                         q, (cur - lst + lq->size) % lq->size, lq->size - 1,
                         cur, lst);
            }
            // 尽力恢复：置 error 让 feed 线程退出 calculate 忙等循环
            // （否则它们永远不会回到 notify 检查点，后续更新永久死锁），
            // 重置回调与队列，避免后续帧叠加卡死。
            ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
            epd_lcd_line_source_cb(NULL, NULL);
            epd_lcd_frame_done_cb(NULL, NULL);
            // 等 feed 线程退出后排空信号量，防止残留计数假放行
            vTaskDelay(pdMS_TO_TICKS(50));
            for (int i = 0; i < NUM_RENDER_THREADS; i++) {
                while (xSemaphoreTake(ctx->feed_done_smphr[i], 0) == pdTRUE) {}
            }
            break;
        }

        for (int i = 0; i < NUM_RENDER_THREADS; i++) {
            // [InkWord 诊断] 有限等待：feed 线程卡死时吐状态而非永久阻塞
            // [run73] 1s tick 采样：吐 ISR 计数器时间序列 + CKV/STV 电平，
            // 区分「EOF 骤停」（LCD 引擎 halt）vs「渐慢」（产消追尾）
            int waited_ms = 0;
            bool timed_out = false;
            while (xSemaphoreTake(ctx->feed_done_smphr[i], pdMS_TO_TICKS(1000)) != pdTRUE) {
                waited_ms += 1000;
                uint32_t vs = 0, eo = 0, ba = 0;
                int ckv = -1, stv = -1;
                epd_lcd_dbg_counters(&vs, &eo, &ba);
                epd_lcd_dbg_pins(&ckv, &stv);
                ESP_LOGW("epd_lcd",
                         "  tick+%ds k=%d i=%d prep=%d cons=%d vsync=%u eof=%u batches=%u ckv=%d stv=%d",
                         waited_ms / 1000, k, i,
                         atomic_load(&ctx->lines_prepared),
                         atomic_load(&ctx->lines_consumed),
                         (unsigned)vs, (unsigned)eo, (unsigned)ba, ckv, stv);
                if (waited_ms >= 25000) {
                    ESP_LOGE("epd_lcd",
                             "feed_done[%d] 超时！k=%d prepared=%d consumed=%d err=0x%X",
                             i, k, atomic_load(&ctx->lines_prepared),
                             atomic_load(&ctx->lines_consumed), ctx->error);
                    timed_out = true;
                    break;
                }
            }
            if (!timed_out) {
                continue;
            }
            // [InkWord 修复] 置 error 让残余 feed 线程退出忙等（帧虽
            // 完成，但末尾几行的生产者可能仍卡在队列满的忙等里，
            // 没有 notify 检查点）；等待退出后排空信号量防残留假放行。
            ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
            epd_lcd_line_source_cb(NULL, NULL);
            epd_lcd_frame_done_cb(NULL, NULL);
            vTaskDelay(pdMS_TO_TICKS(50));
            for (int j = 0; j < NUM_RENDER_THREADS; j++) {
                while (xSemaphoreTake(ctx->feed_done_smphr[j], 0) == pdTRUE) {}
            }
            return;
        }

        // [run76 诊断] 每帧消费闭环：eof×4=实际输出行数。1080/270 = 批 2
        // （尾 80 行）真消费；1008/250 = 批边界 DMA 停摆复现（卖家参数下）。
        {
            uint32_t v1 = 0, e1 = 0, b1 = 0;
            uint32_t va_wr = 0, va_rs = 0, va_rb = 0;
            uint32_t va_ar = 0, va_as = 0;
            epd_lcd_dbg_counters(&v1, &e1, &b1);
            epd_lcd_dbg_va(&va_wr, &va_rs, &va_rb);
            epd_lcd_dbg_va2(&va_ar, &va_as);
            ESP_LOGI("epd_lcd", "frame k=%d done: prep=%d cons=%d vsync=%u eof=%u batches=%u va=%u/%u/%u/%u/%u",
                     k, atomic_load(&ctx->lines_prepared),
                     atomic_load(&ctx->lines_consumed),
                     (unsigned)v1, (unsigned)e1, (unsigned)b1,
                     (unsigned)va_wr, (unsigned)va_rs, (unsigned)va_ar,
                     (unsigned)va_as, (unsigned)va_rb);
        }

        ctx->current_frame++;

        // make the watchdog happy.
        vTaskDelay(0);
    }

    epd_lcd_line_source_cb(NULL, NULL);
    epd_lcd_frame_done_cb(NULL, NULL);

    epd_set_mode(0);
}

void epd_push_pixels_lcd(RenderContext_t *ctx, short time, int color) {
    epd_set_mode(1);
    ctx->current_frame = 0;
    epd_lcd_frame_done_cb((frame_done_func_t)handle_lcd_frame_done, ctx);
    if (color == 0) {
        epd_lcd_line_source_cb((line_cb_func_t)&fill_line_black, ctx);
    } else if (color == 1) {
        epd_lcd_line_source_cb((line_cb_func_t)&fill_line_white, ctx);
    } else {
        epd_lcd_line_source_cb((line_cb_func_t)&fill_line_noop, ctx);
    }
    epd_lcd_start_frame();
    xSemaphoreTake(ctx->frame_done, portMAX_DELAY);
    epd_set_mode(0);
}

#define int_min(a, b) (((a) < (b)) ? (a) : (b))
__attribute__((optimize("O3")))
void IRAM_ATTR lcd_calculate_frame(RenderContext_t *ctx, int thread_id) {
    uint8_t* input_line = ctx->feed_line_buffers[thread_id];

    LineQueue_t *lq = &ctx->line_queues[thread_id];
    int l = 0;

    lut_func_t input_calc_func = get_lut_function(ctx);

    // if there is an error, start the frame but don't feed data.
    if (ctx->error) {
        memset(ctx->line_threads, 0, ctx->lines_total);
        epd_lcd_line_source_cb((line_cb_func_t)&retrieve_line_isr, ctx);
        epd_lcd_start_frame();
        ESP_LOGW("epd_lcd", "draw frame draw initiated, but an error flag is set: %X", ctx->error);
        return;
    }

    assert(input_calc_func != NULL);

    // line must be able to hold 2-pixel-per-byte or 1-pixel-per-byte data
    memset(input_line, 0x00, ctx->display_width);


    EpdRect area = ctx->area;
    int min_y, max_y, bytes_per_line, _ppB;
    const uint8_t *ptr_start;
    get_buffer_params(ctx, &bytes_per_line, &ptr_start, &min_y, &max_y, &_ppB);

    assert(area.width == ctx->display_width && area.x == 0 && !ctx->error);

    // index of the line that triggers the frame output when processed
    int trigger_line = int_min(63, max_y - min_y);

    while (l = atomic_fetch_add(&ctx->lines_prepared, 1), l < ctx->lines_total) {
        // [InkWord 修复] 循环头检查 error：do_update 超时路径置位后，
        // 忙等中的 feed 线程必须能退出，否则永久死锁（无 notify 检查点）。
        if (ctx->error) return;

        ctx->line_threads[l] = thread_id;

        // queue is sufficiently filled to fill both bounce buffers, frame
        // can begin
        if (l - min_y == trigger_line) {
            // [InkWord 诊断] 帧启动点（IRAM 内用 ROM printf，避免 flash 访问）
            esp_rom_printf("[epd_lcd] start_frame @ l=%d thread=%d prepared=%d\n",
                           l, thread_id, atomic_load(&ctx->lines_prepared));
            epd_lcd_line_source_cb((line_cb_func_t)&retrieve_line_isr, ctx);
            epd_lcd_start_frame();
        }

        if (l < min_y || l >= max_y ||
            (ctx->drawn_lines != NULL &&
             !ctx->drawn_lines[l - area.y])) {
            uint8_t *buf = NULL;
            while (buf == NULL) {
                // break in case of errors
                if (ctx->error & EPD_DRAW_EMPTY_LINE_QUEUE) {
                    printf("on err 1: %d %d\n", ctx->lines_prepared, ctx->lines_consumed);
                    lq_reset(lq);
                    return;
                };
                // [run75 修复] 帧已输出完毕：残留行无意义，丢弃退出
                if (ctx->frame_output_done) {
                    lq_reset(lq);
                    return;
                }

                buf = lq_current(lq);
                // [InkWord 修复] 纯自旋会饿死同核 idle（TWDT 触发），
                // 让出 1 tick；队列 32 行缓冲余量充足，ISR 消费期间
                // bb 缓冲继续供线。
                if (buf == NULL) vTaskDelay(0);
            }
            memset(buf, 0x00, lq->element_size);
            lq_commit(lq);
            continue;
        }

        uint32_t *lp = (uint32_t *)input_line;
        const uint8_t *ptr = ptr_start + bytes_per_line * (l - min_y);

        Cache_Start_DCache_Preload((uint32_t)ptr, ctx->display_width, 0);

        lp = (uint32_t *)ptr;

        uint8_t *buf = NULL;
        while (buf == NULL) {
            // break in case of errors
            if (ctx->error & EPD_DRAW_EMPTY_LINE_QUEUE) {
                lq_reset(lq);
                printf("on err 2: %d %d\n", ctx->lines_prepared, ctx->lines_consumed);
                return;
            };
            // [run75 修复] 帧已输出完毕：残留行无意义，丢弃退出
            if (ctx->frame_output_done) {
                lq_reset(lq);
                return;
            }

            buf = lq_current(lq);
            // [InkWord 修复] 同上：自旋让出，避免饿死同核任务。
            if (buf == NULL) vTaskDelay(0);
        }

        (*input_calc_func)(lp, buf, ctx->conversion_lut, ctx->display_width);

        lq_commit(lq);
    }
}

#endif
