/**
 * @file inkword_features.h
 * @brief 功能裁剪开关单一权威（开源通用化 Phase 1，2026-09-08）
 *
 * 五轴裁剪体系：offline env（inkword-s3-offline）经 build_flags 注入
 * -D INKWORD_FEATURE_*=0 关闭对应子系统；实现体由 src/stubs/ 桩源
 * 顶替（build_src_filter 切换），头文件原样保留——业务文件调用点
 * 零改动（类型定义天然可用，桩函数返回失败值走既有降级路径）。
 *
 * 契约（全功能档零变化铁律）：
 *   - 宏未定义（全功能 env）时一律兜底 1，行为与裁剪体系引入前
 *     逐比特一致（#if INKWORD_FEATURE_* 侵入点仅 offline 档生效）；
 *   - 裁剪轴与源文件/桩的对应关系：
 *     AUDIO → audio_player/es8311/mic_recorder/ui_sfx
 *     CLOUD → sync_client/sync_session/ota_manager/audio_sync
 *     AI    → chat_mode/voice_search（chat_ui 随 chat_mode 编出）
 *     WIFI  → wifi_manager/wifi_config_ui/ble_provision
 *     LAN   → lan_display_server/lan_proto
 *   - 依赖方向：AI/LAN 依赖 CLOUD（sync_client 的 base_url/device_key
 *     与 wifi 链路）；offline 档五轴全关，全功能档五轴全开，中间
 *     组合（如仅关 AI）当前未验证，按需实测。
 *
 * 兜底范式同 EPD_PANEL_DEFAULT_ID（epd_panel.h）/INKWORD_BLE_PROVISION
 * （main.cpp）：#ifndef + 默认值，子 env 注入覆盖。
 */
#ifndef INKWORD_FEATURES_H
#define INKWORD_FEATURES_H

/* clang-format off */
#ifndef INKWORD_FEATURE_AUDIO
#define INKWORD_FEATURE_AUDIO 1   /* 音频子系统：I2S 播放/录音/codec/提示音 */
#endif

#ifndef INKWORD_FEATURE_CLOUD
#define INKWORD_FEATURE_CLOUD 1   /* 云同步：sync_client/sync_session/OTA/音频补齐 */
#endif

#ifndef INKWORD_FEATURE_AI
#define INKWORD_FEATURE_AI 1      /* AI 对话/语音查词（依赖 CLOUD/WIFI） */
#endif

#ifndef INKWORD_FEATURE_WIFI
#define INKWORD_FEATURE_WIFI 1    /* Wi-Fi 联网/配网/BLE 配网 */
#endif

#ifndef INKWORD_FEATURE_LAN
#define INKWORD_FEATURE_LAN 1     /* LAN 直传/AP 门户（依赖 WIFI） */
#endif
/* clang-format on */

#endif /* INKWORD_FEATURES_H */
