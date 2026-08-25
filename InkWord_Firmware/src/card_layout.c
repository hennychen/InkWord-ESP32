/**
 * @file card_layout.c
 * @brief 卡片版式分派实现（协议与语义见 card_layout.h）
 */
#include "card_layout.h"

#include <string.h>

card_layout_t card_layout_from_payload(const char *payload_type)
{
    if (!payload_type || !payload_type[0]) return CARD_LAYOUT_WORD;
    if (strcmp(payload_type, "qa-card") == 0)   return CARD_LAYOUT_QA;
    if (strcmp(payload_type, "poem-card") == 0) return CARD_LAYOUT_POEM;
    return CARD_LAYOUT_WORD;   /* 未知类型保守回退现版式 */
}
