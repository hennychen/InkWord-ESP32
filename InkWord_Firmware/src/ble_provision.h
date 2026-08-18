/**
 * @file ble_provision.h
 * @brief BLE 配网服务（NimBLE GATT，App 发现 / 配网 / 状态查询）
 *
 * BLE 仅承担发现与配网，内容传输仍走 Wi-Fi HTTP（/api/display）。
 *
 * 广播：设备名 InkWord-XXXX + manufacturer data（厂商 ID 0x02E5 +
 *       6 字节载荷 [协议版本, Wi-Fi 状态, IP×4]），App 扫广播即得
 *       设备 IP（发现闭环主通道，不依赖 mDNS）。
 * GATT 服务 cc5a0001-...：
 *   - creds (write)        JSON {"ssid","pass"} -> 复用 wifi_connect_async
 *   - status (read+notify) JSON {"state","ip"}；拒绝时 notify error+reason
 *   - scan   (write+notify) 写任意字节触发设备侧扫描，逐 AP 流式推送
 *
 * 协议常量与 App 端 InkWord_App/lib/core/epd_protocol.dart 单点同步。
 *
 * 并发约束（对齐 HTTP 端 503 语义）：软键盘配网 UI 激活或正在连接中
 * 时拒绝配网；Wi-Fi 扫描阻塞 1~2 秒，必须在独立任务执行（BLE 回调
 * 里只置标志，绝不阻塞）。
 */
#ifndef INKWORD_BLE_PROVISION_H
#define INKWORD_BLE_PROVISION_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 BLE 配网服务（幂等；在 wifi_manager_init 之后调用，
 *        失败仅打印告警，不阻塞主流程——设备仍可走 Portal 配网）。
 * @return 0 成功或已初始化；<0 初始化失败。
 */
int ble_provision_init(void);

/**
 * @brief 服务是否已就绪（GATT 已启动，广播运行中）。
 */
bool ble_provision_ready(void);

/**
 * @brief Wi-Fi 状态 / IP 变化后刷新广播数据与 status 订阅端。
 *        内部与状态轮询任务共用去抖快照，重复调用安全。
 */
void ble_adv_update_state(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_BLE_PROVISION_H */
