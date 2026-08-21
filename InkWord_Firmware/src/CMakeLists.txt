# src/CMakeLists.txt (ESP-IDF "main" component, used by PlatformIO espidf framework)
# Task F-03: 注册主组件并声明组件级依赖。
# P3: 三级点阵字库 bin（~631KB）。
# 注意：当前 framework=arduino 下 PlatformIO 不读本文件（构建用 platformio.ini
# 的 board_build.embed_files）；本段供未来 ESP-IDF CMake 构建迁移用，
# 迁移后符号为 _binary_cjk_font_data_bin_start（无 src_ 路径前缀，
# cjk_font.c 的 extern 需同步调整，见其文件头注释）。
idf_component_register(
    SRCS
        "debug_log.c"
        "epd_driver.cpp"
        "audio_player.c"
        "button_handler.c"
        "haptic.c"
        "storage_manager.c"
        "refresh_scheduler.c"
        "word_parser.c"
        "srs_engine.c"
        "learning_state.c"
        "study_mode_machine.c"
        "standby_page.c"
        "cjk_font.c"
        "cjk_text.c"
        "reader_engine.c"
        "wifi_manager.c"
        "wifi_config_ui.c"
        "sync_client.c"
        "ota_manager.c"
        "lan_display_server.cpp"
        "ble_provision.cpp"
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
