/**
 * @file sync_session.h
 * @brief 云端同步会话编排（P2 巨石拆分：自 main.cpp 迁出）
 *
 * 职责：凭据恢复（NVS → sync_client）、MAC 幂等注册与 401 换钥自愈
 * （ADR-001 §五）、上报队列 flush、静默心跳会话（RTC TIMER 唤醒的
 * 极简路径）、后台任务（LAN 拉起/心跳/天气/OTA，10 分钟周期）。
 * 分层：sync_client 为纯 HTTP 客户端，本模块是其编排层（可依赖
 * power/wifi/ota/standby/lan 等服务模块，反向则禁止）。
 */
#ifndef INKWORD_SYNC_SESSION_H
#define INKWORD_SYNC_SESSION_H

#ifdef __cplusplus
extern "C" {
#endif

/* 后端 API Base URL（P2 上报闭环）：部署时 -D INKWORD_API_BASE=... 覆盖，
 * 或经 NVS "inkword"/"api_url" 覆盖（配网 UI 扩展后可写）；
 * http: 前缀自动走明文 TCP（本地开发后端，见 sync_client fill_cfg） */
#ifndef INKWORD_API_BASE
#define INKWORD_API_BASE "https://api.einkword.com"
#endif

/** 同步凭据恢复：NVS "inkword"/{api_url, dev_key} → sync_set_*；
 *  setup 与静默心跳会话两路径各调一次 */
void sync_credentials_load(void);

/** 启动后台任务（LAN 拉起/注册/心跳/天气/OTA，10 分钟周期；
 *  setup 尾部调；栈 6KB 优先级 4 见实现） */
void sync_background_task_start(void);

/** 静默心跳会话（RTC TIMER 唤醒路径：校时/上报/OTA 后回睡，不返回） */
void silent_heartbeat_session(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_SYNC_SESSION_H */
