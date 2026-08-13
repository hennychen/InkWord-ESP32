/**
 * @file storage_manager.c
 * @brief SD 卡读写实现 (Task F-12)，SPI 模式 + FAT 文件系统。
 */
#include "storage_manager.h"
#include "debug_log.h"
#include "gpio_config.h"

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>

static const char *TAG = "STORAGE";
static bool s_mounted = false;
static sdmmc_card_t *s_card = NULL;

int storage_init(void)
{
    if (s_mounted) return 0;

    /* 1. 初始化 SPI 总线与 SDSPI 设备 */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_MOSI_PIN,
        .miso_io_num = SD_MISO_PIN,
        .sclk_io_num = SD_SCLK_PIN,
        .max_transfer_sz = 4096,
    };
    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        LOG_E("spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return -1;
    }

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = SD_CS_PIN;
    slot_cfg.host_id = host.slot;

    /* 2. 挂载 FAT 文件系统到 VFS */
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        LOG_E("failed to mount SD card: %s", esp_err_to_name(ret));
        return -1;
    }

    s_mounted = true;
    LOG_I("SD card mounted at %s", SD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);
    return 0;
}

int storage_read_text(const char *path, char *out_buf, size_t buf_size)
{
    if (!s_mounted || !out_buf || buf_size == 0) return -1;

    FILE *f = fopen(path, "r");
    if (!f) {
        LOG_E("open failed: %s", path);
        return -1;
    }
    size_t n = fread(out_buf, 1, buf_size - 1, f);
    out_buf[n] = '\0';
    fclose(f);
    return (int)n;
}

bool storage_file_exists(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0);
}

void storage_list_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        LOG_E("opendir failed: %s", dir);
        return;
    }
    struct dirent *ent;
    LOG_I("--- listing %s ---", dir);
    while ((ent = readdir(d)) != NULL) {
        LOG_I("  %s", ent->d_name);
    }
    closedir(d);
}

const char *storage_mount_point(void)
{
    return SD_MOUNT_POINT;
}
