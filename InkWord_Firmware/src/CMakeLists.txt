# src/CMakeLists.txt (ESP-IDF "main" component, used by PlatformIO espidf framework)
# Task F-03: 注册主组件并声明组件级依赖。
# P3: 三级点阵字库 bin（~631KB）。
# P5: 深睡与定时唤醒电源管理。
# 注意：当前 framework=arduino 下 PlatformIO 不读本文件（构建用 platformio.ini
# 的 board_build.embed_files）；本段供未来 ESP-IDF CMake 构建迁移用，
# 迁移后符号为 _binary_cjk_font_data_bin_start（无 src_ 路径前缀，
# cjk_font.c 的 extern 需同步调整，见其文件头注释）。
idf_component_register(
    SRCS
        "debug_log.c"
        "epd_driver.cpp"
        "epd_panel.c"
        "panels/panel_depg0370_uc8253.cpp"
        "panels/panel_e042a13_ssd1619.cpp"   # Phase 6：4.2" 三色面板单元（SSD1619 手写序列，与 arduino 自动递归编译双轨同步）
        "panels/panel_wf0270_ssd1680.cpp"    # 2.7" 三色面板单元（SSD1680 手写序列，维峰 WF0270，双轨同步）
        "panels/panel_gdew027c44_il91874.cpp" # 2.7" 三色面板单元（IL91874 手写序列，真机验证，双轨同步）
        "panels/panel_wft0290.cpp"           # 2.9" 128x296 BW 骨架（WFT0290CZ10，bring-up 待硬件，2026-08-23）
        "panels/panel_opm021eb.cpp"          # 2.13" 122x250 BW 骨架（OPM021EB，bring-up 待硬件，2026-08-23）
        "panels/panel_e042a13bw.cpp"         # 4.2" 400x300 BW 骨架（E042A13-A0 黑白版，bring-up 待硬件，2026-08-23）
        "audio_player.c"
        "audio_sync.c"       # P0C：词条发音音频同步（http/TLS 分流同 ota_fill_cfg）
        "es8311.c"           # ES8311 codec I2C 寄存器驱动（2026-08-24，取代 MAX98357A+INMP441）
        "i2c_bus.c"           # v1.2 T2.6：公共 I2C 总线（共享 38/39 + 互斥锁）
        "max17048.c"          # v1.2 T2.6：电量计驱动（代码就绪待硬件）
        "mic_recorder.c"      # P1：ES8311 ADC 跟读录音+multipart 上传（I2S 全双工，DOUT=GPIO11）
        "chat_mode.c"        # P2B：AI 对话五态状态机（录音 10s/上传/下载/播放，临时视图）
        # libhelix MP3 解码器（P0A 2026-08-24 源码内嵌，arduino 框架
        # 经 src/ 递归编译，本清单为 ESP-IDF CMake 迁移双轨同步；
        # 上游来源与裁剪说明见 mp3/README.md）
        "mp3/bitstream.c"
        "mp3/buffers.c"
        "mp3/dct32.c"
        "mp3/dequant.c"
        "mp3/dqchan.c"
        "mp3/huffman.c"
        "mp3/hufftabs.c"
        "mp3/imdct.c"
        "mp3/mp3dec.c"
        "mp3/mp3tabs.c"
        "mp3/polyphase.c"
        "mp3/scalfact.c"
        "mp3/stproc.c"
        "mp3/subband.c"
        "mp3/trigtabs.c"
        "button_handler.c"
        "haptic.c"
        "ui_sfx.c"         # v1.1 T1.6：UI 提示音（key/rate/mode/err 异步短样本）
        "storage_manager.c"
        "refresh_scheduler.c"
        "word_parser.c"
        "srs_engine.c"
        "learning_state.c"
        "quiz_session.c"     # v1.2 T2.1：四选一出题核心（纯 C，native-test，泛化回调）
        "daily_plan.c"       # v1.2 T2.4：每日目标量与今日任务判据（NVS set_daily）
        "settings_ui.c"      # v1.2 T2.5：设置页覆盖层 + 发音/震动/字号门控（set_*）
        "deck_manager.c"     # v1.3 T3.1：词书卡组（manifest 扫描/活跃切换，rd_* 隔离配套）
        "study_mode_machine.c"
        "layout_profile.c"   # P5+Phase5：L4 布局档位层（档位→字库级映射）
        "standby_page.c"
        "cjk_font.c"
        "cjk_font_sd.c"   # v1.4 T4.5：SD 卡组子集字库级联（主集 miss → 子集）
        "cjk_text.c"
        "reader_engine.c"
        "wifi_manager.c"
        "wifi_config_ui.c"
        "menu_ui.c"          # P 快捷菜单：长按中进入的功能菜单（收藏/模式/配网/门户/LAN/设备信息）
        "sync_client.c"
        "ota_manager.c"
        "lan_display_server.cpp"
        "ble_provision.cpp"
        "power_manager.c"
        "main.cpp"
    EMBED_FILES
        "cjk_font_data.bin"
    INCLUDE_DIRS
        "."
    REQUIRES
        esp_event
        nvs_flash
        esp_wifi
        esp_http_client
        esp_http_server
        mdns
        esp_https_ota
        driver
        fatfs
        spiffs
        json
)
