/**
 * @file lan_proto.h
 * @brief LAN 直传协议 v2 帧分类与头解析（T2.3 修 B3，纯 C 无硬件依赖）
 *
 * 协议谱系（版本协商一次性定型，PANEL_COMPAT_DESIGN §12.2）：
 *   v1    无头裸 body，单平面 epd_fb_size() 字节（B/W，bit=1 白；
 *         多平面面板余平面设备侧补零）——历史脚本/客户端兼容窗
 *   v1.5  无头裸 body，双平面 epd_fb_total() 字节（[0]=B/W + [1]=accent；
 *         2026-08-22 彩色传图；长度判别，BW 面板自然退化 v1）
 *   v2    8B 帧头 + body：magic 'I''W'(0x49 0x57) + proto_ver(1B) +
 *         bpp(1B：1=单平面 / 2=B/W+accent 双平面，即每像素 2 位) +
 *         W/H(2B each 大端，面板物理像素) + body((W+7)/8*H*bpp 字节)
 *
 * 无歧义长度分流：8B 头使 v2 总长恒比 v1/v1.5 裸长多 8（面板平面
 * 字节数远大于 8），classify 先按 content_len 分流，无 peek 需求。
 * 独立成模块的理由同 selftest_diff：display handler 在 C++ 编译单元，
 * 判别逻辑 native-test 覆盖（合法四路径 + 错尺寸/坏版本/bpp 超限拒绝）。
 */
#ifndef INKWORD_LAN_PROTO_H
#define INKWORD_LAN_PROTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LAN_V2_MAGIC0   0x49    /* 'I' */
#define LAN_V2_MAGIC1   0x57    /* 'W' */
#define LAN_PROTO_VER   2

typedef enum {
    LAN_FRAME_REJECT = 0,   /* 长度不匹配任何合法格式（err：四种合法长度） */
    LAN_FRAME_V1_BW,        /* 无头单平面（content_len == fb_size） */
    LAN_FRAME_V15_COLOR,    /* 无头双平面（content_len == fb_total） */
    LAN_FRAME_V2_BW,        /* 8B 头 + 单平面（content_len == 8 + fb_size） */
    LAN_FRAME_V2_COLOR,     /* 8B 头 + 双平面（content_len == 8 + fb_total） */
} lan_frame_kind_t;

/**
 * @brief 按 content_len 分流帧格式（无 peek，无歧义）
 * @param fb_size  单平面字节数（epd_fb_size()）
 * @param fb_total 全平面字节数（epd_fb_total()；BW 面板 == fb_size）
 */
lan_frame_kind_t lan_frame_classify(size_t content_len, size_t fb_size,
                                    size_t fb_total);

/**
 * @brief 解析 v2 8B 帧头（classify 判为 V2_* 后调用）
 * @param hdr          8 字节头
 * @param panel_w/h    面板物理像素（错尺寸拒绝——计划验收明确文案）
 * @param plane_count  面板平面数（1=BW / 2=带 accent；bpp=2 超限拒绝）
 * @param body_len     出参：头声明的 body 字节数
 * @param err          出参：失败原因静态串（HTTP 400 文案用）
 * @return 0 成功；非 0 失败（err 必写）
 */
int lan_v2_header_parse(const uint8_t hdr[8], int panel_w, int panel_h,
                        int plane_count, size_t *body_len, const char **err);

#ifdef __cplusplus
}
#endif

#endif /* INKWORD_LAN_PROTO_H */
