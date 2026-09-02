/**
 * @file word_loader.c
 * @brief 活跃词库装载链路（v1.3 T3.1 / T1.3 自 main.cpp 迁出，修 A1）
 *
 * 迁移口径（T1.3，2026）：函数体零改动；MAX_WORDS 容量口径注释与
 * INKWORD_DEMO_WORDS 兜底定义随迁（装载链路外无消费方）。
 * 词池 s_word_pool/s_word_cap 归本模块所有——渲染层经 word_parser_get
 * 取词条，池指针不出模块边界。
 */
#include "word_loader.h"

#include <stdio.h>

#include "debug_log.h"
#include "gpio_config.h"       /* SD_MOUNT_POINT */
#include "word_parser.h"       /* word_parser_load* / WordEntry */
#include "deck_manager.h"      /* deck_manager_active_* / scan */
#include "cjk_font_sd.h"       /* v1.4 T4.5：卡组子集字库级联 */
#include "storage_manager.h"   /* storage_file_exists */
#include "catalog_index.h"     /* catalog_build */

#include "esp_heap_caps.h"     /* heap_caps_malloc / MALLOC_CAP_SPIRAM */

static const char *TAG = "WLOAD";

/* 词库容量（PRD §7.2 容量红线 2026-08-20 解除）：词池迁 PSRAM 后
 * 上限 4000 词（词库扩展四字段后 sizeof(WordEntry)≈1096B，
 * 4000 词 ≈ 4.2MB；与阅读器单书上限 4MB 并发最坏 ≈ 8.2MB——仅
 * “满词库+4MB 大书”同时存在时才触顶，实际书多在 1-2MB 且词池
 * 分配失败时逐半降级兼容）。实际分配不足时 word_loader_init 内
 * 逐级降级。
 * v1.4 T4.2 重估：口径 = 单活跃卡组（load_active_words 只装当前
 * deck，4000 上限即单 deck 词条上限；多卡组并存只多占 SD，不多占
 * PSRAM——词池/解析 DOM/学习状态数组均随活跃组重载复用） */
#define MAX_WORDS   4000

/* 演示词库开关：inkword-s3-demo 环境置 1；无 SD 词库时加载内嵌 5 词，
 * 用于学习页按键（翻词/SET 遮蔽/RST 回首）的整机验证；
 * 正式构建保持 0，无词库仍走待机页（用户定稿行为） */
#ifndef INKWORD_DEMO_WORDS
#define INKWORD_DEMO_WORDS 0
#endif

/* 词池：PSRAM 堆分配（原 DRAM 静态数组仅容 64 词；与 reader_engine
 * 书缓冲同策略 MALLOC_CAP_SPIRAM，setup 内 storage_init 后分配） */
static WordEntry *s_word_pool = NULL;
static int        s_word_cap = 0;   /* 实际分配容量（降级后 < MAX_WORDS） */

/* 活跃词库装载（v1.3 T3.1，setup 与 deck_flow_switch 共用链路）：
 * 活跃卡组文件 → SD words.json → 内嵌兜底 三级递降；任一级成功
 * 即返回（内嵌常在 rodata，末级必达）。返回词条数（<0 全失败）。 */
static int load_active_words(void)
{
    /* v1.4 T4.5 字库子集级联：随活跃卡组装载/切换同步装载该组 SD 子集
     * 字库（/sdcard/fonts/deck_<id>.bin，无文件=主集已覆盖的正常路径，
     * 静默通过；旧组子集在 load 内部先退场）。置于词条链路之前：
     * 子集只依赖 deck_active，与哪级词条源命中无关 */
    cjk_font_sd_load(deck_manager_active_id());

    const char *deck_file = deck_manager_active_file();
    if (deck_file) {
        int n = word_parser_load(deck_file, s_word_pool, s_word_cap);
        if (n > 0) {
            LOG_I("deck DB loaded: %d entries (%s)", n, deck_file);
            return n;
        }
        LOG_E("deck load failed: %s, fallback", deck_file);
    }

    const char *word_file = SD_MOUNT_POINT "/words.json";
    if (storage_file_exists(word_file)) {
        int n = word_parser_load(word_file, s_word_pool, s_word_cap);
        if (n > 0) {
            LOG_I("word DB loaded: %d entries", n);
            return n;
        }
    }

    /* 出厂内嵌兜底（2026-08-23）：无 SD 卡开箱即用。词库随固件烧入
     * rodata（platformio.ini embed_files，生成链见
     * tools/default_vocab），load_mem 零拷贝直吃。SD/卡组词库存在时
     * 优先（可更新、可携带 cloudId）；内嵌版本无 cloudId（本地词条，
     * 评分/收藏不上报），在线同步/导出路径下发的词库才携带 */
    extern const uint8_t _binary_src_default_words_json_start[];
    extern const uint8_t _binary_src_default_words_json_end[];
    return word_parser_load_mem(
        (const char *)_binary_src_default_words_json_start,
        (size_t)(_binary_src_default_words_json_end -
                 _binary_src_default_words_json_start),
        s_word_pool, s_word_cap);
}

/* 词库装载 + 目录索引（设计 §A1）：装载链路统一挂载点，setup 与
 * deck_flow_switch 共用（词库切换时 catalog_build 内部 free 重建） */
int load_words_with_catalog(void)
{
    int n = load_active_words();
    catalog_build();
    return n;
}

/* setup 步骤 6 整段（T1.3 自 main.cpp setup 函数化迁出，缩进/注释
 * 原样）：词池逐半降级分配 → 卡组扫描 → 三级装载 → 演示词兜底 */
void word_loader_init(void)
{
    /* 加载词库（词池 PSRAM 化，2026-08-20）：按 MAX_WORDS 逐半降级
     * 分配，与阅读器书缓冲共享 8MB Octal；全部分配失败（极小概率）
     * 置空容量，词库空走待机页 */
    for (int cap = MAX_WORDS; cap > 0 && !s_word_pool; cap /= 2) {
        s_word_pool = (WordEntry *)heap_caps_malloc(
            (size_t)cap * sizeof(WordEntry), MALLOC_CAP_SPIRAM);
        if (s_word_pool) s_word_cap = cap;
        else LOG_W("word pool alloc %d entries failed, halving", cap);
    }
    LOG_I("word pool: %d entries x %uB = %uKB PSRAM",
          s_word_cap, (unsigned)sizeof(WordEntry),
          (unsigned)((size_t)s_word_cap * sizeof(WordEntry) / 1024));

    /* v1.3 T3.1：卡组扫描（无 manifest 退化为单默认卡组，开箱行为
     * 不变）；装载走 load_active_words 三级递降链路（与切书共用） */
    deck_manager_scan();
    if (s_word_pool) {
        int n = load_words_with_catalog();
        LOG_I("word DB ready: %d entries", n);
    } else {
        LOG_W("word pool alloc failed, no word DB");
    }
#if INKWORD_DEMO_WORDS
    /* 测试构建：内嵌词库也被排除时（如裁剪验证）的最后一道演示词 */
    if (s_word_pool && word_parser_get_count() == 0) {
        word_parser_load_demo(s_word_pool, s_word_cap);
        catalog_build();    /* 演示词路径同建索引（装载尾部口径统一） */
    }
#endif
}
