/**
 * @file settings_keys.h
 * @brief NVS 键单一权威表（P2b）
 *
 * 命名空间与键名唯一登记处：改键名 = 改此处，旧键数据作废（无迁移
 * 路径，烧录重配）。键名 ≤15 字符（NVS 上限）；主命名空间 NVS_NS
 * （"inkword"），Wi-Fi 凭据独立 NVS_NS_WIFI（"wifi"）。
 *
 * scope 拼接族（卡组隔离，非默认组加 deck id 后缀）：基名/前缀在
 * 本表登记，拼接由各模块构造函数完成——
 *   learning_state lr_key：默认组 NVS_KEY_LR_STATE，其余
 *     NVS_KEY_LR_ST_PFX "<id>"（rd_* 后缀同先例）；
 *   daily_plan dp_goal_key：默认组 NVS_KEY_SET_DAILY，其余
 *     NVS_KEY_SD_PFX "<id>"；
 *   reader_engine rd_key：基名 + s_scope 后缀（reader_set_progress_scope）。
 * 值类型与默认值见各调用处；u8 设置族默认值集中在 settings_ui.c。
 */
#ifndef INKWORD_SETTINGS_KEYS_H
#define INKWORD_SETTINGS_KEYS_H

/* ---- 命名空间 ---- */
#define NVS_NS              "inkword"   /* 主命名空间（除 Wi-Fi 凭据外全部） */
#define NVS_NS_WIFI         "wifi"      /* Wi-Fi 凭据独立隔离 */

/* ---- 云端同步（sync_session；str） ---- */
#define NVS_KEY_API_URL     "api_url"   /* 后端 base URL 覆盖（缺省=编译宏） */
#define NVS_KEY_DEV_KEY     "dev_key"   /* 设备认证 ApiKey（MAC 注册回写） */

/* ---- Wi-Fi（wifi_manager；str） ---- */
#define NVS_KEY_WIFI_SSID   "ssid"
#define NVS_KEY_WIFI_PASS   "pass"

/* ---- 设置族（settings_ui / epd_driver；u8 除注明） ---- */
#define NVS_KEY_SET_PANEL   "set_panel" /* 运行期面板名（str；epd_driver 读） */
#define NVS_KEY_SET_AUDIO   "set_audio"
#define NVS_KEY_SET_HAPTIC  "set_haptic"
#define NVS_KEY_SET_FONT    "set_font"
#define NVS_KEY_SET_WORD    "set_word"
#define NVS_KEY_SET_BOLD    "set_bold"
#define NVS_KEY_SET_QUIZGRID "set_quizgrid"
#define NVS_KEY_SET_ROT     "set_rot"
#define NVS_KEY_SET_VOL     "set_vol"
#define NVS_KEY_SET_DAILY   "set_daily" /* 每日目标量默认组（sd_<id> 族） */
#define NVS_KEY_SD_PFX      "sd_"       /* 每日目标量卡组键前缀（u8） */

/* ---- 长按快捷键（shortcut_map；u8 = sk_action_t 枚举，缺省=出厂） ---- */
#define NVS_KEY_SK_UP       "sk_up"     /* 上长按 */
#define NVS_KEY_SK_DN       "sk_dn"     /* 下长按 */
#define NVS_KEY_SK_LF       "sk_lf"     /* 左长按 */
#define NVS_KEY_SK_RT       "sk_rt"     /* 右长按 */
#define NVS_KEY_SK_SET      "sk_set"    /* SET 长按 */
#define NVS_KEY_SK_RST      "sk_rst"    /* RST 长按 */

/* ---- 学习状态（study_mode_machine / deck_manager / learning_state） ---- */
#define NVS_KEY_LAST_MODE   "last_mode" /* 上次学习模式（u8，恢复用） */
#define NVS_KEY_DECK_ACTIVE "deck_active" /* 活动卡组（str；缺省=默认组） */
#define NVS_KEY_SET_EXAM    "set_exam"  /* 考试日期 ymd（u32；daily_plan） */
#define NVS_KEY_LR_STATE    "lr_state"  /* LR 状态 blob 默认组 */
#define NVS_KEY_LR_ST_PFX   "lr_st_"    /* LR 状态 blob 卡组键前缀 */
#define NVS_KEY_LR_STATS    "lr_stats"  /* 累计统计 blob（跨词库保留） */
#define NVS_KEY_LR_STDECK   "lr_stdeck" /* 跨词库学习统计 blob */

/* ---- 阅读进度（reader_engine；rd_key 基名 + scope 后缀） ---- */
#define NVS_KEY_RD_SIG      "rd_sig"    /* 书签名（u32；书变化失效判据） */
#define NVS_KEY_RD_FONT     "rd_font"   /* 字号档（u8） */
#define NVS_KEY_RD_PAGE     "rd_page"   /* 页码（u32） */

/* ---- 待机页（standby_page） ---- */
#define NVS_KEY_WX_CACHE    "wx_cache"  /* 天气缓存 blob（weather_info_t） */
#define NVS_KEY_WX_TS       "wx_ts"     /* 天气缓存时间戳（u32） */
#define NVS_KEY_SLP_EPOCH   "slp_ep0"   /* 入睡自治钟基准 Unix 秒（i64） */
#define NVS_KEY_SLP_RTC     "slp_rtc0"  /* 入睡时刻系统 RTC 原始值（i64） */

#endif /* INKWORD_SETTINGS_KEYS_H */
