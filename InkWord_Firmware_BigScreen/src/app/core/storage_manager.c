/**
 * @file storage_manager.c
 * @brief SPIFFS 书库实现（大屏阅读器阶段；桩实现自 app_stubs.c 移除）
 *
 * esp_vfs_spiffs 挂 storage 分区后走 POSIX 文件接口（opendir/
 * fopen），书库枚举读 /storage/books/。读书走 reader_engine 的
 * 自管加载（预检大小 + PSRAM 分配），本模块 storage_read_text
 * 仅作通用文本读（词库回退路径兼容语义）。
 */
#include "storage_manager.h"
#include "debug_log.h"

#include "esp_spiffs.h"
#include "esp_err.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "STOR";

#define BOOKS_DIR "/storage/books"

int storage_init(void)
{
    esp_vfs_spiffs_conf_t cfg = {
        .base_path = "/storage",
        .partition_label = "storage",
        .max_files = 8,
        .format_if_mount_failed = true,   /* 首次烧录分区未格式化 */
    };
    esp_err_t err = esp_vfs_spiffs_register(&cfg);
    if (err != ESP_OK) {
        LOG_E("SPIFFS 挂载失败: %s", esp_err_to_name(err));
        return -1;
    }
    size_t total = 0, used = 0;
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK)
        LOG_I("SPIFFS 就绪：%u/%u 字节", (unsigned)used, (unsigned)total);
    return 0;
}

int storage_read_text(const char *path, char *out_buf, size_t buf_size)
{
    if (!path || !out_buf || buf_size == 0) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(out_buf, 1, buf_size - 1, f);
    fclose(f);
    out_buf[n] = '\0';
    return (int)n;
}

/* 书籍扩展名白名单（.txt / .md；.md 不做格式清洗，章节检测走 MD 策略） */
static bool book_ext_ok(const char *name)
{
    size_t l = strlen(name);
    if (l > 4 && strcasecmp(name + l - 4, ".txt") == 0) return true;
    if (l > 3 && strcasecmp(name + l - 3, ".md") == 0) return true;
    return false;
}

int storage_books_list(char names[][STORAGE_BOOK_NAME_MAX], int max)
{
    if (max > STORAGE_SHELF_MAX) max = STORAGE_SHELF_MAX;
    if (max <= 0) return 0;

    DIR *d = opendir(BOOKS_DIR);
    if (!d) return 0;   /* 未挂载/无目录 = 空书库（非错误） */
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        if (!book_ext_ok(e->d_name)) continue;
        size_t dl = strlen(e->d_name);
        if (dl >= STORAGE_BOOK_NAME_MAX)
            continue;   /* 超长名跳过（截断后路径不可开，无意义） */
        memcpy(names[n], e->d_name, dl + 1);   /* 含 NUL；memcpy 避开
                                                  * snprintf 截断告警 */
        n++;
    }
    closedir(d);

    /* 文件名升序插入排序（n ≤ 16，书架顺序稳定可预期；行拷贝走
     * memmove/memcpy——snprintf 同对象 src/dst 触发 -Werror=restrict） */
    for (int i = 1; i < n; i++) {
        char key[STORAGE_BOOK_NAME_MAX];
        memcpy(key, names[i], sizeof(key));
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], key) > 0) {
            memmove(names[j + 1], names[j], STORAGE_BOOK_NAME_MAX);
            j--;
        }
        memmove(names[j + 1], key, sizeof(key));
    }
    return n;
}

void storage_book_path(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s/%s", BOOKS_DIR, name ? name : "");
}
