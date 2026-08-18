/**
 * @file standby_page.h
 * @brief 无词库待机页（《传习录》引文独占：居中引文 + 右下出处）
 *
 * 词库为空（word_parser_get_count()==0）时设备默认显示的整页待机界面
 * （引文独占构图，用户 2026-08-18 定稿：仅显示《传习录》，星期/日期/
 * 农历/月年/时间均不显示）：
 *   - 引文块：cjk_font 子集楷体 Bold 24px 点阵，8 字/行 x 5 行，
 *     行距 8px（行高 32，松排版）；屏幕水平居中 + 带内垂直居中；
 *     每 5 分钟轮换一条（24 条循环）
 *   - 出处：右下角右对齐 "——王阳明《传习录》"（同字库静态署名）
 *   - 时间无效时引文留白（仅出处）；天气不显示（拉取/NVS 缓存/
 *     校时兑底数据链路保留，随时可加回）
 *
 * 并发模型（单一写者纪律，零新增 FreeRTOS 任务）：
 *   - 所有绘制仅发生在主循环任务（loop -> standby_tick / standby_render_full）；
 *   - 按键回调只置标志位（standby_on_button），绝不直接绘图；
 *   - 后台任务仅投递天气数据（standby_weather_update，短临界区拷贝）。
 *
 * 时间源：应用层自治时钟（系统 time()/settimeofday 在本机损坏，弃用）：
 *         esp_timer 单调钟换算 Unix 秒；HTTP Date 响应头（主，80 端口不受
 *         运营商 UDP 123 劫持影响）+ 后端 serverTime 校时兜底；epoch 落在
 *         [2025,2100] 之外视为无效，引文留白（仅出处）。
 */
#ifndef INKWORD_STANDBY_PAGE_H
#define INKWORD_STANDBY_PAGE_H

#include <stdbool.h>
#include "button_handler.h"
#include "sync_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化待机页：从 NVS 恢复上次天气缓存（3 小时内有效）。
 *        在 setup 词库加载之后、首次渲染之前调用一次。
 */
void standby_init(void);

/**
 * @brief 待机页是否激活（词库为空即激活，无需显式进入/退出）。
 */
bool standby_is_active(void);

/**
 * @brief 待机页周期心跳，由 Arduino loop() 每秒调用一次。
 *        内部自判：每 5 分钟轮换引文（变化即整页全局刷新，无局刷路径；
 *        STANDBY_QUOTE_INTERVAL_S 可 build_flags 覆盖）、
 *        联网时经 HTTP Date 头校时
 *        （未同步 30s 重试 / 成功后 6h 校准）、消费后台投递的天气
 *        （仅校时兜底与 NVS 持久化，不绘制）。
 *        非待机状态或页面被接管（配网/LAN）时零开销返回。
 */
void standby_tick(void);

/**
 * @brief 待机页按键入口（按键回调上下文调用，只置标志不绘图）。
 *        语义：长按中=Wi-Fi 配置 / 长按上=清残影全刷 / 长按左=AP 门户 /
 *              长按右=LAN 接收页 / 短按中=立即拉取天气；其余忽略。
 */
void standby_on_button(nav_key_t id, button_event_t event);

/**
 * @brief 投递最新天气（后台任务或主循环调用，线程安全）。
 *        数据经短临界区拷贝，由下一次 standby_tick 在主循环消费
 *        （NVS 持久化 + serverTime 校时兜底，当前页面不绘制天气）；
 *        若携带 serverTime 则校时（偏差 >60s 才重置基准）。
 */
void standby_weather_update(const weather_info_t *w);

/**
 * @brief 整页重绘 + 全刷（进入待机页 / 从配网、LAN 页退出恢复时调用）。
 *        配网页或 LAN 接收页激活期间自动跳过绘制。
 */
void standby_render_full(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_STANDBY_PAGE_H */
