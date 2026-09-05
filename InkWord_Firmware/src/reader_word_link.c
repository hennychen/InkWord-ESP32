/**
 * @file reader_word_link.c
 * @brief 阅读中生词联动实现（2026-09-05 阅读器增强阶段四b）
 *
 * 扫描当前页文本，提取英文单词（按空格/标点分词）和中文单字，
 * 在词库中线性查找匹配项。匹配到的生词列表供阅读器菜单消费。
 *
 * 跳转闪卡经 study_mode_seek（模式切 FLASH + 游标定位），
 * 阅读器进度由 NVS rd_page 保持（回来自动恢复）。
 */
#include "reader_word_link.h"
#include "reader_engine.h"
#include "word_parser.h"
#include "study_mode_machine.h"
#include "debug_log.h"

#include <string.h>
#include <ctype.h>
#include <stdio.h>   /* snprintf（wl->meaning 填充，缺此头为隐式声明 UB 风险） */

static const char *TAG = "WORDLINK";

/* UTF-8 首字节判断 */
static inline bool is_utf8_cont(uint8_t c) { return (c & 0xC0) == 0x80; }

/* 判断 UTF-8 码点是否为 CJK 统一汉字（U+4E00..U+9FFF） */
static inline bool is_cjk(uint32_t cp)
{
    return cp >= 0x4E00 && cp <= 0x9FFF;
}

/* 判断是否为词分隔符（空格/标点/换行/数字间分隔） */
static inline bool is_word_sep(uint32_t cp)
{
    if (cp < 0x80) return !isalnum((int)cp);
    /* CJK 字符本身作为分隔（中文按字查词） */
    if (is_cjk(cp)) return true;
    /* 全角空格/标点 */
    if (cp == 0x3000 || cp == 0x3001 || cp == 0x3002 ||
        cp == 0xFF0C || cp == 0xFF1A || cp == 0xFF1B) return true;
    /* 通用标点区 U+2000..U+206F */
    if (cp >= 0x2000 && cp <= 0x206F) return true;
    return false;
}

/* 词库线性查找（按 text 精确匹配，大小写敏感） */
static int find_word_in_dict(const char *text, int len)
{
    int n = word_parser_get_count();
    for (int i = 0; i < n; i++) {
        const WordEntry *we = word_parser_get(i);
        if (!we) continue;
        if ((int)strlen(we->text) == len &&
            strncmp(we->text, text, (size_t)len) == 0)
            return i;
    }
    return -1;
}

/* 去重：检查 out 中是否已有相同 word_index */
static bool is_duplicate(const word_link_t *out, int count, int word_index)
{
    for (int i = 0; i < count; i++)
        if (out[i].word_index == word_index) return true;
    return false;
}

int reader_word_link_scan(int page, word_link_t *out, int max_links)
{
    if (!out || max_links <= 0) return 0;
    const char *text = reader_engine_get_text();
    uint32_t text_len = reader_engine_get_text_len();
    if (!text || text_len == 0) return 0;
    int total_pages = reader_page_count();
    if (page < 0 || page >= total_pages) return 0;

    /* 当前页文本范围 */
    uint32_t start = reader_engine_page_offset(page);
    uint32_t end = (page + 1 < total_pages)
                 ? reader_engine_page_offset(page + 1)
                 : text_len;
    if (start >= text_len) start = text_len;
    if (end > text_len) end = text_len;

    int links = 0;
    int char_off = 0;
    uint32_t pos = start;

    while (pos < end && links < max_links) {
        /* 解码当前 UTF-8 字符 */
        const uint8_t *b = (const uint8_t *)(text + pos);
        uint32_t cp = 0;
        int n = 1;
        if (*b < 0x80) { cp = *b; n = 1; }
        else if ((*b & 0xE0) == 0xC0) {
            cp = *b & 0x1F; n = 2;
        } else if ((*b & 0xF0) == 0xE0) {
            cp = *b & 0x0F; n = 3;
        } else if ((*b & 0xF8) == 0xF0) {
            cp = *b & 0x07; n = 4;
        } else {
            pos++; char_off++; continue;  /* 坏字节跳过 */
        }
        if (pos + n > end) break;
        for (int i = 1; i < n; i++) {
            if ((b[i] & 0xC0) != 0x80) { cp = 0xFFFD; break; }
            cp = (cp << 6) | (b[i] & 0x3F);
        }
        pos += n;

        if (is_word_sep(cp)) { char_off++; continue; }

        /* 提取一个"词"：英文连续字母 / CJK 单字 */
        uint32_t word_start = pos - n;
        uint32_t word_end = pos;
        if (is_cjk(cp)) {
            /* CJK 单字查词（2-4 字词在词库中匹配） */
            /* 先看后续是否也是 CJK，尝试 2/3/4 字组合 */
            for (int wlen = 4; wlen >= 2; wlen--) {
                uint32_t try_end = word_end;
                for (int k = 1; k < wlen && try_end < end; k++) {
                    const uint8_t *nb = (const uint8_t *)(text + try_end);
                    int nn = 0;
                    uint32_t ncp = 0;
                    if (*nb < 0x80) { nn = 1; ncp = *nb; }
                    else if ((*nb & 0xE0) == 0xC0) { ncp = *nb & 0x1F; nn = 2; }
                    else if ((*nb & 0xF0) == 0xE0) { ncp = *nb & 0x0F; nn = 3; }
                    else { break; }
                    if (try_end + nn > end) break;
                    if (!is_cjk(ncp)) break;
                    try_end += nn;
                }
                int actual_chars = 0;
                uint32_t scan = word_end;
                for (int k = 0; k < wlen && scan < end; k++) {
                    const uint8_t *sb = (const uint8_t *)(text + scan);
                    int sn = 1;
                    if (*sb >= 0x80) {
                        if ((*sb & 0xE0) == 0xC0) sn = 2;
                        else if ((*sb & 0xF0) == 0xE0) sn = 3;
                        else if ((*sb & 0xF8) == 0xF0) sn = 4;
                    }
                    if (scan + sn > end) break;
                    uint32_t scp = 0;
                    if (*sb < 0x80) scp = *sb;
                    else if ((*sb & 0xE0) == 0xC0) scp = *sb & 0x1F;
                    else if ((*sb & 0xF0) == 0xE0) scp = *sb & 0x0F;
                    else scp = *sb & 0x07;
                    for (int j = 1; j < sn; j++) {
                        scp = (scp << 6) | (((const uint8_t *)text)[scan + j] & 0x3F);
                    }
                    if (!is_cjk(scp)) break;
                    scan += sn;
                    actual_chars++;
                }
                if (actual_chars >= 2) {
                    int wlen_bytes = (int)(scan - word_start);
                    int idx = find_word_in_dict(text + word_start, wlen_bytes);
                    if (idx >= 0 && !is_duplicate(out, links, idx)) {
                        word_link_t *wl = &out[links];
                        memset(wl, 0, sizeof(*wl));
                        int copy_len = wlen_bytes < (int)sizeof(wl->text) - 1
                                     ? wlen_bytes : (int)sizeof(wl->text) - 1;
                        memcpy(wl->text, text + word_start, copy_len);
                        wl->text[copy_len] = '\0';
                        const WordEntry *we = word_parser_get(idx);
                        if (we) {
                            snprintf(wl->meaning, sizeof(wl->meaning), "%s", we->meaning);
                        }
                        wl->word_index = idx;
                        wl->char_offset = char_off;
                        links++;
                        pos = scan;
                        /* 跳过后续 CJK 字符 */
                        while (pos < end) {
                            const uint8_t *pb = (const uint8_t *)(text + pos);
                            int pn = 1;
                            if (*pb >= 0x80) {
                                if ((*pb & 0xE0) == 0xC0) pn = 2;
                                else if ((*pb & 0xF0) == 0xE0) pn = 3;
                                else if ((*pb & 0xF8) == 0xF0) pn = 4;
                            }
                            if (pos + pn > end) break;
                            uint32_t pcp = 0;
                            if (*pb < 0x80) pcp = *pb;
                            else if ((*pb & 0xE0) == 0xC0) pcp = *pb & 0x1F;
                            else if ((*pb & 0xF0) == 0xE0) pcp = *pb & 0x0F;
                            else pcp = *pb & 0x07;
                            for (int j = 1; j < pn; j++)
                                pcp = (pcp << 6) | (((const uint8_t *)text)[pos + j] & 0x3F);
                            if (!is_cjk(pcp)) break;
                            pos += pn;
                            char_off++;
                        }
                        goto next_char;
                    }
                }
            }
            /* 单字 CJK 不进词库（粒度太细），跳过 */
            char_off++;
            goto next_char;
        }

        /* 英文单词：连续字母序列 */
        while (pos < end) {
            const uint8_t *nb = (const uint8_t *)(text + pos);
            if (*nb >= 0x80) break;  /* 非 ASCII = 分隔 */
            if (!isalpha(*nb)) break;
            pos++;
        }
        word_end = pos;
        int wlen = (int)(word_end - word_start);
        if (wlen >= 2) {   /* 至少 2 字母才查词 */
            int idx = find_word_in_dict(text + word_start, wlen);
            if (idx >= 0 && !is_duplicate(out, links, idx)) {
                word_link_t *wl = &out[links];
                memset(wl, 0, sizeof(*wl));
                int copy_len = wlen < (int)sizeof(wl->text) - 1
                             ? wlen : (int)sizeof(wl->text) - 1;
                memcpy(wl->text, text + word_start, copy_len);
                wl->text[copy_len] = '\0';
                const WordEntry *we = word_parser_get(idx);
                if (we) {
                    snprintf(wl->meaning, sizeof(wl->meaning), "%s", we->meaning);
                }
                wl->word_index = idx;
                wl->char_offset = char_off;
                links++;
            }
        }
        char_off++;
next_char:
        ;
    }
    LOG_I("page %d word scan: %d links", page, links);
    return links;
}

int reader_word_link_jump(int word_index)
{
    if (word_index < 0 || word_index >= word_parser_get_count()) return -1;
    /* 保存当前阅读进度（reader_render_page 已自动保存，此处冗余保险） */
    /* 切到闪卡模式并定位到目标词 */
    study_mode_seek(word_index);
    LOG_I("jump to word index %d", word_index);
    return 0;
}
