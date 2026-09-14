
set(app_sources "src/epdiy.c"
                "src/render.c"
                "src/output_lcd/render_lcd.c"
                "src/output_lcd/lcd_driver.c"
                "src/output_i2s/render_i2s.c"
                "src/output_i2s/rmt_pulse.c"
                "src/output_i2s/i2s_data_bus.c"
                "src/output_common/lut.c"
                "src/output_common/line_queue.c"
                "src/output_common/render_context.c"
                "src/font.c"
                "src/displays.c"
                "src/board_specific.c"
                "src/builtin_waveforms.c"
                "src/highlevel.c"
                "src/board/tps65185.c"
                "src/board/pca9555.c"
                "src/board/epd_board.c"
                "src/board/epd_board_common.c"
                "src/board/epd_board_lilygo_t5_47.c"
                "src/board/epd_board_v2_v3.c"
                "src/board/epd_board_v4.c"
                "src/board/epd_board_v5.c"
                "src/board/epd_board_v6.c"
                "src/board/epd_board_v7.c"
)


# Can also use IDF_VER for the full esp-idf version string but that is harder to parse. i.e. v4.1.1, v5.0-beta1, etc
if (${IDF_VERSION_MAJOR} GREATER 4)
    idf_component_register(SRCS ${app_sources} INCLUDE_DIRS "src/" REQUIRES driver esp_timer esp_adc esp_lcd)
else()
    idf_component_register(SRCS ${app_sources} INCLUDE_DIRS "src/" REQUIRES esp_adc_cal esp_timer esp_lcd)
endif()

# formatting specifiers maybe incompatible between idf versions because of different int definitions
component_compile_options(-Wno-error=format= -Wno-format)

# [InkWord 降噪] 库内已知警告压制（项目源码仍全量开警告，见 platformio.ini）：
#   - unused 系：卖家魔改 epd_board_v7.c / tps65185.c 后残留的空置变量/函数
#     （I2C PMIC 链被注释所致），非功能问题；
#   - deprecated：lcd_driver.c 中 gdma_new_channel/gdma_set_transfer_ability
#     在 IDF 5.5 弃用，上游 master 已迁移但牵连 rmt_compat 重构，最小补丁
#     策略下暂不跟进（Phase 2 统一升上游时一并处理）。
component_compile_options(-Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function -Wno-deprecated-declarations)
set_source_files_properties("src/output_common/lut.c" PROPERTIES COMPILE_OPTIONS -mno-fix-esp32-psram-cache-issue)
