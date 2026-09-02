/**
 * @file lan_proto.c
 * @brief LAN 直传协议 v2 帧分类与头解析实现（T2.3，纯 C 无硬件依赖）
 */
#include "lan_proto.h"

lan_frame_kind_t lan_frame_classify(size_t content_len, size_t fb_size,
                                    size_t fb_total)
{
    if (content_len == fb_size) return LAN_FRAME_V1_BW;
    if (fb_total > fb_size && content_len == fb_total) return LAN_FRAME_V15_COLOR;
    if (content_len == 8 + fb_size) return LAN_FRAME_V2_BW;
    if (fb_total > fb_size && content_len == 8 + fb_total) return LAN_FRAME_V2_COLOR;
    return LAN_FRAME_REJECT;
}

int lan_v2_header_parse(const uint8_t hdr[8], int panel_w, int panel_h,
                        int plane_count, size_t *body_len, const char **err)
{
    if (hdr[0] != LAN_V2_MAGIC0 || hdr[1] != LAN_V2_MAGIC1) {
        *err = "bad magic";
        return -1;
    }
    if (hdr[2] != LAN_PROTO_VER) {
        *err = "unsupported proto_ver";
        return -2;
    }
    const int bpp = hdr[3];
    if (bpp != 1 && bpp != 2) {
        *err = "bad bpp (must be 1 or 2)";
        return -3;
    }
    const int w = (hdr[4] << 8) | hdr[5];
    const int h = (hdr[6] << 8) | hdr[7];
    if (w != panel_w || h != panel_h) {
        *err = "frame size mismatch";
        return -4;
    }
    if (bpp == 2 && plane_count < 2) {
        *err = "panel has no accent plane";
        return -5;
    }
    *body_len = (size_t)((w + 7) / 8) * (size_t)h * (size_t)bpp;
    return 0;
}
