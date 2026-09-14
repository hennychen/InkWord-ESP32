#pragma once
// LAN 图片上传显示入口（适配里程碑 M1）。
// 实现见 lan_image.c：WiFi SoftAP + esp_http_server，浏览器侧完成解码/
// 灰度/Floyd–Steinberg 抖动并上传 1920x1080 1bit 打包裸流（MSB first，
// bit1=白），设备解包入 epdiy framebuffer 后全屏 GC16 显示。
#ifdef __cplusplus
extern "C" {
#endif

void lan_image_task(void* arg);

#ifdef __cplusplus
}
#endif
