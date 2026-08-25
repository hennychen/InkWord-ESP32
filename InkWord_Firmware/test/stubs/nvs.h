/**
 * @file nvs.h
 * @brief NVS 内存 mock（native-test 专用，-I test/stubs 优先命中）
 *
 * 仅覆盖 daily_plan.c 引用的 u8/u32 接口（blob 不 mock——被测源不用）。
 * kv 表单实例：声明在头，实现由 test_daily_plan.c 提供（native 单
 * program 链接，避免头文件 static inline 多实例状态分裂）。
 */
#ifndef STUB_NVS_H
#define STUB_NVS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int nvs_handle_t;
typedef int esp_err_t;

#define ESP_OK              0
#define ESP_ERR_NVS_NOT_FOUND 0x1102

#define NVS_READONLY  0
#define NVS_READWRITE 1

/** 清空 mock 键值表（每个测试用例开头调用） */
void nvs_mock_reset(void);

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *out);
void      nvs_close(nvs_handle_t h);
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out);
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v);
esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out);
esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t v);
esp_err_t nvs_commit(nvs_handle_t h);

#ifdef __cplusplus
}
#endif
#endif /* STUB_NVS_H */
