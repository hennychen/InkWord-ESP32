/**
 * @file catalog_index.c
 * @brief 教材目录索引实现（设计 §A1，协议见 catalog_index.h 头注）
 *
 * 两遍构建（年级分桶 → 桶内 unit 分桶），桶内词条保持词库原序
 * （导出顺序即教材顺序，qsort 只排桶头不动 entries）。静态桶表
 * ≈9KB BSS + entries 8KB PSRAM，4000 词实测毫秒级。
 */
#include "catalog_index.h"
#include "word_parser.h"     /* WordEntry（类型依赖，native 链 word_parser.c 同链） */
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>

#define CATALOG_OTHER_GRADE "(其他)"
#define CATALOG_NO_UNIT     "(未分类)"

/* ---- 静态索引状态（BSS，release 清零） ---- */

static catalog_bucket_t s_grades[CATALOG_GRADE_MAX];
static int              s_grade_n = 0;
static catalog_bucket_t s_units[CATALOG_GRADE_MAX][CATALOG_UNIT_MAX];
static int              s_unit_n[CATALOG_GRADE_MAX];
static uint16_t        *s_entries = NULL;   /* 词库序号数组（总长=词条数） */
static int              s_entry_total = 0;
static int32_t          s_u_cursor[CATALOG_GRADE_MAX][CATALOG_UNIT_MAX]; /* 构建期填充游标 */
static bool             s_ready = false;

/* ---- 小工具 ---- */

/* 安全 UTF-8 截断拷贝：不切多字节序列中间（尾部 continuation 回退） */
static void copy_trunc(char *dst, int dst_sz, const char *src)
{
    int n = (int)strlen(src);
    if (n >= dst_sz) n = dst_sz - 1;
    while (n > 0 && (src[n] & 0xC0) == 0x80) n--;   /* 回退 continuation 字节 */
    memcpy(dst, src, (size_t)n);
    dst[n] = '\0';
}

/* 不区分大小写子串查找（newlib 无 strcasestr，自实现） */
static const char *str_case_str(const char *hay, const char *needle)
{
    int nl = (int)strlen(needle);
    for (const char *p = hay; *p; p++) {
        int i = 0;
        while (i < nl && p[i] &&
               (p[i] | 0x20) == (needle[i] | 0x20)) i++;
        if (i == nl) return p;
    }
    return NULL;
}

/* 不区分大小写定长前缀比较（unit_num 的 Starter 回看用） */
static bool eq_ci(const char *p, const char *word, int wl)
{
    for (int i = 0; i < wl; i++)
        if (!p[i] || (p[i] | 0x20) != (word[i] | 0x20)) return false;
    return true;
}

void catalog_unit_name_of(const char *src, char *out, int out_sz)
{
    if (!src || !src[0]) {
        copy_trunc(out, out_sz, CATALOG_NO_UNIT);
        return;
    }
    /* 最后一个 "unit" 起截到结尾并回看 Starter 前缀（2026-08-28 单元
     * 细分）："人教版 九年级 Unit 5 What…" → "Unit 5 What…"（前置
     * 版本/年级信息由 grade 层承载）；"Starter Unit 1 Good morning!"
     * → 整段保留（Starter 与正课单元不合并）；无 "unit" 整串去重 */
    const char *hit = NULL;
    for (const char *q = src; (q = str_case_str(q, "unit")) != NULL; q += 4)
        hit = q;
    const char *name = hit ? hit : src;
    if (hit && hit - src >= 8 && eq_ci(hit - 8, "starter", 7) &&
        (hit[-1] == ' ' || hit[-1] == '\t'))
        name = hit - 8;      /* "starter unit n…"：起点回退含 Starter */
    while (*name == ' ' || *name == '\t') name++;
    copy_trunc(out, out_sz, name);
}

/* ---- 年级排序：预定义序表模糊包含优先，"(其他)" 恒末位 ---- */

static const char *kGradeOrder[] = {
    "小学",                                        /* 古诗词集（最小学段） */
    "七年级上", "七年级下", "七年级",
    "八年级上", "八年级下", "八年级",
    "九年级上", "九年级下", "九年级",
    "考纲拓展",                                    /* 不隶属教材单元的超纲词 */
    "中考", "高考", "初中", "高中",
};
#define GRADE_ORDER_N ((int)(sizeof(kGradeOrder) / sizeof(kGradeOrder[0])))

static int grade_rank(const char *name)
{
    if (strcmp(name, CATALOG_OTHER_GRADE) == 0) return GRADE_ORDER_N + 1;
    for (int i = 0; i < GRADE_ORDER_N; i++)
        if (strstr(name, kGradeOrder[i])) return i;
    return GRADE_ORDER_N;   /* 未匹配：字典序尾置 */
}

/* qsort 比较器临时项（名称 + rank，排序后写回桶表） */
typedef struct { char name[CATALOG_NAME_MAX]; int rank; } grade_sort_t;

static int cmp_grade(const void *a, const void *b)
{
    const grade_sort_t *ga = a, *gb = b;
    if (ga->rank != gb->rank) return ga->rank < gb->rank ? -1 : 1;
    return strcmp(ga->name, gb->name);
}

/* Unit 排序键（2026-08-28 单元细分）：数值优先，其余字典序，
 * "(未分类)" 恒末位。"[Starter] Unit N <话题名>" 均入数值序
 * （Starter 键 = N，正课键 = N+1000：Starter 1/2/3 排 Unit 1 前
 * 且各自升序）；话题名后缀不阻数值序（"Unit 1 My name's Gina."） */
static bool unit_num(const char *name, int *out)
{
    const char *p = name;
    bool starter = false;
    if (eq_ci(p, "starter", 7) && (p[7] == ' ' || p[7] == '\t')) {
        starter = true;
        p += 7;
        while (*p == ' ' || *p == '\t') p++;
    }
    const char *u = "unit";
    for (int i = 0; i < 4; i++)
        if (!p[i] || (p[i] | 0x20) != (u[i] | 0x20)) return false;
    p += 4;
    while (*p == ' ' || *p == '\t') p++;
    if (*p < '0' || *p > '9') return false;
    int v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    *out = starter ? v : v + 1000;   /* Starter 正序在前，正课大偏移在后 */
    return true;
}

typedef struct {
    char    name[CATALOG_NAME_MAX];
    int     num;
    bool    is_unit;
    int32_t len;      /* 计数（桶排序时随 name 一起搬） */
} unit_sort_t;

static int cmp_unit(const void *a, const void *b)
{
    const unit_sort_t *ua = a, *ub = b;
    bool ea = strcmp(ua->name, CATALOG_NO_UNIT) == 0;
    bool eb = strcmp(ub->name, CATALOG_NO_UNIT) == 0;
    if (ea != eb) return ea ? 1 : -1;
    if (ua->is_unit != ub->is_unit) return ua->is_unit ? -1 : 1;
    if (ua->is_unit && ua->num != ub->num)
        return ua->num < ub->num ? -1 : 1;
    return strcmp(ua->name, ub->name);
}

/* 词的 grade 归桶名（trim 空串 → "(其他)"） */
static void grade_name_of(const char *grade, char *out, int out_sz)
{
    if (!grade) grade = "";
    while (*grade == ' ' || *grade == '\t') grade++;
    if (!grade[0]) { copy_trunc(out, out_sz, CATALOG_OTHER_GRADE); return; }
    copy_trunc(out, out_sz, grade);
}

/* grade 名定位桶下标：精确匹配 → "(其他)" 兑底（溢出/被逐名归入）；
 * 无兑底桶返回 -1（防御，Pass 1 兑底改造保证理论不到） */
static int locate_grade(const char *grade, char *name, int name_sz, int gn)
{
    grade_name_of(grade, name, name_sz);
    for (int j = 0; j < gn; j++)
        if (strcmp(s_grades[j].name, name) == 0) return j;
    for (int j = 0; j < gn; j++)
        if (strcmp(s_grades[j].name, CATALOG_OTHER_GRADE) == 0) return j;
    return -1;
}

/* ---- 构建 ---- */

int catalog_build_from(int n, const void *words_v)
{
    const WordEntry *words = words_v;
    catalog_release();
    if (n <= 0 || !words) { s_ready = true; return 0; }

    /* Pass 1：distinct 年级名收集（容量满：末槽改造为 "(其他)"
     * 兜底桶，溢出年级统一走 Pass 2 兜底归入） */
    grade_sort_t gtmp[CATALOG_GRADE_MAX];
    int gn = 0;
    char name[CATALOG_NAME_MAX];
    for (int i = 0; i < n; i++) {
        grade_name_of(words[i].grade, name, sizeof(name));
        bool dup = false;
        for (int j = 0; j < gn; j++)
            if (strcmp(gtmp[j].name, name) == 0) { dup = true; break; }
        if (dup) continue;
        if (gn == CATALOG_GRADE_MAX) {
            copy_trunc(name, sizeof(name), CATALOG_OTHER_GRADE);
            for (int j = 0; j < gn; j++)
                if (strcmp(gtmp[j].name, name) == 0) { dup = true; break; }
            if (!dup) {   /* 无兜底桶：末槽改造（原槽位词走 Pass 2 兜底） */
                copy_trunc(gtmp[gn - 1].name, sizeof(gtmp[gn - 1].name), name);
                gtmp[gn - 1].rank = grade_rank(name);
            }
            break;   /* 后续 distinct 不再收集（全部 Pass 2 兜底） */
        }
        copy_trunc(gtmp[gn].name, sizeof(gtmp[gn].name), name);
        gtmp[gn].rank = grade_rank(gtmp[gn].name);
        gn++;
    }
    if (gn == 0) { s_ready = true; return 0; }

    qsort(gtmp, (size_t)gn, sizeof(gtmp[0]), cmp_grade);

    /* 桶头落表（start/len 先置 0，Pass 2 填充） */
    for (int j = 0; j < gn; j++)
        copy_trunc(s_grades[j].name, sizeof(s_grades[j].name), gtmp[j].name);

    /* entries 分配（PSRAM；native stub 走 malloc） */
    s_entries = heap_caps_malloc((size_t)n * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!s_entries) return -1;

    /* Pass 2：词库序单遍计数（grade 定位 + unit distinct/计数）。
     * unit 名暂存 s_units[g][*].name（首现序）、计数在 .len；桶间词
     * 交错，entries 此阶段不写（见 Pass 3 游标填充）；容量满：
     * 查/建 "(未分类)" 兑底桶（建 = 末槽改名，计数沿用并入） */
    for (int i = 0; i < n; i++) {
        int g = locate_grade(words[i].grade, name, sizeof(name), gn);
        if (g < 0) continue;                     /* 防御：无兑底桶（理论不到） */
        catalog_unit_name_of(words[i].source, name, sizeof(name));
        int u;
        for (u = 0; u < s_unit_n[g]; u++)
            if (strcmp(s_units[g][u].name, name) == 0) break;
        if (u == s_unit_n[g]) {                  /* 新 distinct 单元名 */
            if (s_unit_n[g] < CATALOG_UNIT_MAX) {
                copy_trunc(s_units[g][u].name, sizeof(s_units[g][u].name), name);
                s_units[g][u].len = 0;
                s_unit_n[g]++;
            } else {                             /* 容量满：归/建兑底桶 */
                for (u = 0; u < s_unit_n[g]; u++)
                    if (strcmp(s_units[g][u].name, CATALOG_NO_UNIT) == 0) break;
                if (u == s_unit_n[g]) {          /* 无兑底：末槽改名（计数并入） */
                    u = s_unit_n[g] - 1;
                    copy_trunc(s_units[g][u].name, sizeof(s_units[g][u].name),
                               CATALOG_NO_UNIT);
                }
            }
        }
        s_units[g][u].len++;
    }

    /* Pass 3：各年级 unit 排序（数值/字典序，计数随 name 搬）+
     * 前缀和定位桶 start（桶间连续，交错写入不再成立的问题根治） */
    int32_t total = 0;
    for (int j = 0; j < gn; j++) {
        int un = s_unit_n[j];
        unit_sort_t utmp[CATALOG_UNIT_MAX];
        for (int m = 0; m < un; m++) {
            copy_trunc(utmp[m].name, sizeof(utmp[m].name), s_units[j][m].name);
            utmp[m].is_unit = unit_num(utmp[m].name, &utmp[m].num);
            utmp[m].len = s_units[j][m].len;
        }
        qsort(utmp, (size_t)un, sizeof(utmp[0]), cmp_unit);

        s_grades[j].start = total;
        for (int m = 0; m < un; m++) {
            copy_trunc(s_units[j][m].name, sizeof(s_units[j][m].name), utmp[m].name);
            s_units[j][m].len = utmp[m].len;
            s_units[j][m].start = total;
            s_u_cursor[j][m] = total;            /* 填充游标初值 = start */
            total += utmp[m].len;
        }
        s_grades[j].len = total - s_grades[j].start;
    }
    s_entry_total = total;

    /* Pass 3b：游标填充（按词库序遍历，unit 内天然保持教材原序/稳定；
     * 桶定位与 Pass 2 同规则，被逐名/溢出统一兑底一致命中） */
    for (int i = 0; i < n; i++) {
        int g = locate_grade(words[i].grade, name, sizeof(name), gn);
        if (g < 0) continue;
        catalog_unit_name_of(words[i].source, name, sizeof(name));
        int u;
        for (u = 0; u < s_unit_n[g]; u++)
            if (strcmp(s_units[g][u].name, name) == 0) break;
        if (u == s_unit_n[g])                    /* 被逐名：归兑底桶 */
            for (u = 0; u < s_unit_n[g]; u++)
                if (strcmp(s_units[g][u].name, CATALOG_NO_UNIT) == 0) break;
        if (u == s_unit_n[g]) continue;          /* 防御（理论不到） */
        s_entries[s_u_cursor[g][u]++] = (uint16_t)i;
    }

    s_grade_n = gn;
    s_ready = true;
    return gn;
}

int catalog_build(void)
{
    /* 词池首址 = get(0)（数组连续，word_parser 契约）；0 词传 NULL */
    const WordEntry *pool = word_parser_get_count() > 0 ? word_parser_get(0) : NULL;
    return catalog_build_from(word_parser_get_count(), pool);
}

void catalog_release(void)
{
    if (s_entries) heap_caps_free(s_entries);
    s_entries = NULL;
    s_entry_total = 0;
    s_grade_n = 0;
    memset(s_unit_n, 0, sizeof(s_unit_n));
    memset(s_grades, 0, sizeof(s_grades));
    memset(s_units, 0, sizeof(s_units));
    s_ready = false;
}

bool          catalog_ready(void)       { return s_ready; }
int           catalog_grade_count(void) { return s_grade_n; }

const catalog_bucket_t *catalog_grade(int g)
{
    return (g >= 0 && g < s_grade_n) ? &s_grades[g] : NULL;
}

const char *catalog_grade_name(int g)
{
    return (g >= 0 && g < s_grade_n) ? s_grades[g].name : CATALOG_OTHER_GRADE;
}

int catalog_unit_count(int g)
{
    return (g >= 0 && g < s_grade_n) ? s_unit_n[g] : 0;
}

const catalog_bucket_t *catalog_unit(int g, int u)
{
    if (g < 0 || g >= s_grade_n || u < 0 || u >= s_unit_n[g]) return NULL;
    return &s_units[g][u];
}

const uint16_t *catalog_unit_entries(int g, int u, int *out_n)
{
    const catalog_bucket_t *b = catalog_unit(g, u);
    if (!b) { if (out_n) *out_n = 0; return NULL; }
    if (out_n) *out_n = b->len;
    return s_entries + b->start;
}
