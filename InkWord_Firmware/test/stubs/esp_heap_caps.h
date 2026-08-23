/* native-test stub：替代 ESP-IDF esp_heap_caps.h（host 内存直接 malloc）。
 * 仅 test_word_parser_native.c 链接时由 -I test/stubs 优先命中。 */
#ifndef STUB_ESP_HEAP_CAPS_H
#define STUB_ESP_HEAP_CAPS_H

#include <stddef.h>

#define MALLOC_CAP_SPIRAM 0x01

void *heap_caps_malloc(size_t size, int caps);
void heap_caps_free(void *ptr);

#endif
