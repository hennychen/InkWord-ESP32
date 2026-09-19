# main 组件注册（espidf 框架）
# 目录职责（方案 §2.1）：
#   main.c                 入口（宏分派：APP 学习闭环 / PROBE bring-up /
#                          LAN 图片上传 / DEMO 卖家复刻，默认安全初始化）
#   driver/                板级 GPIO 镜像 + ES108FC1C1-RHY 面板安全初始化
#   config/                屏幕/总线/VCOM 参数（保守起步值集中管理）
#   hal/                   显示抽象接口骨架（Phase 2 实现，业务不直接调 epdiy）
#   test/                  bring-up 八步序列（BIGSCREEN_PROBE 门控完整路径）
#   app/                   学习闭环业务层（BIGSCREEN_APP 构建，2026-11 迁移）：
#     app/core/            小屏核心零改动移植 + 桩 + study_mode_machine 精简版
#                          + 阅读器族（reader_engine/chapter_index/
#                          bookmark_mgr/storage_manager，2026-09 移植适配）
#     app/input/           按键 GPIO 扫描（IDF 版）+ 串口命令模拟按键
#     app/ui/              大屏词卡页 + 待机页 + 阅读页/阅读菜单（XLARGE 布局）
#     app/data/            内嵌资产：cjk_font_data.bin（点阵字库 ~1.1MB）
#                          + default_words.json（词库 2407 词 ~568KB）
#                          + demo_book.txt（内置演示书）
#   gfx/                   绘图层：小屏 epd_gfx_* API 的 epdiy 版
#                          （1bpp 画布 → 4bpp fb → GC16 全刷）+ FreeSans 字体表
# demo/：卖家 ED060KD1 demo 复刻（BIGSCREEN_SELLER_DEMO 构建；图片头
# 仅 demo_seller.c 单翻译单元 include，非 static const 避免重复链接）
# lan/：LAN 图片上传显示（BIGSCREEN_LAN 构建，适配里程碑 M1）
#
# 学习闭环源/资产仅 BIGSCREEN_APP 构建编入（platformio.ini
# board_build.cmake_extra_args = -DBIGSCREEN_APP=1 传入 CMake 变量；
# 编译宏本身仍由 build_flags -D 注入 SCons 命令行——pio espidf 双通道
# 实测，见 platformio.ini 注释）：app+gfx 源 + 1.7MB 内嵌资产不进
# probe/lan/demo 固件，现有 env 零影响。
# 资产捆绑不走 EMBED_FILES（pio SCons 双前缀路径 bug，见
# app/data/embed_data.S 文件头）——.incbin 自包含 + 手工导出同命名
# 规则的 _binary_* 全局符号。

# 基础源：入口 + 板级驱动（所有 env 共需；main.c 对 test/lan/demo
# 的引用全部宏门控，BIGSCREEN_APP 固件不编入它们——既隔离 1.7MB
# 学习资产，也隔 test 字体表 bidi 警告等 bring-up 遗留噪音）
set(MAIN_SRCS "main.c" "driver/panel_es108fc.c" "driver/temp_monitor.c")

if(NOT BIGSCREEN_APP)
    list(APPEND MAIN_SRCS
        "test/test_basic.c" "test/demo/demo_seller.c"
        "lan/lan_image.c" "lan/waveform_scanq.c")
endif()

set(MAIN_INCLUDE_DIRS "." "driver" "config" "test")

if(BIGSCREEN_TUNE)
    list(APPEND MAIN_SRCS "tune/tune_task.c" "gfx/epd_gfx.c" "lan/waveform_scanq.c")
    list(APPEND MAIN_INCLUDE_DIRS "tune" "gfx" "gfx/fonts" "lan")
endif()

if(BIGSCREEN_APP)
    list(APPEND MAIN_SRCS
        "app/app_main.c"
        "app/core/srs_engine.c" "app/core/word_parser.c"
        "app/core/page_router.c" "app/core/epd_geom.c"
        "app/core/cjk_font.c" "app/core/cjk_text.c"
        "app/core/layout_profile.c" "app/core/learning_state.c"
        "app/core/study_mode_machine.c" "app/core/quotes_app.c"
        "app/core/app_stubs.c"
        "app/core/reader_engine.c" "app/core/chapter_index.c"
        "app/core/bookmark_mgr.c" "app/core/storage_manager.c"
        "app/input/button_handler.c" "app/input/console_cmd.c"
        "app/ui/word_card_ui.c" "app/ui/standby_page.c"
        "app/ui/menu_ui.c" "app/ui/settings_ui.c"
        "app/ui/reader_page.c" "app/ui/reader_menu.c"
        "app/ui/lan_portal.c"
        "gfx/epd_gfx.c"
        # binfast 二值快速波形（waveform_scanq.c）：全刷 3.3s→~0.7s
        # 且更锐（2026-09-17 优化接入；scanq 部分未被引用，链接器
        # gc-sections 裁剪，flash 增量仅 binfast LUT 2KB）
        "lan/waveform_scanq.c"
        "app/data/embed_data.S")
    # 平铺 include（小屏源码风格：#include "xxx.h" 无目录前缀）
    list(APPEND MAIN_INCLUDE_DIRS
        "app/core" "app/input" "app/ui" "gfx" "gfx/fonts")
    # lan/：lan_portal 复用 lan_page.h 上传页（浏览器侧 canvas/FS 抖动
    # run99 页面零改动；产品化网关与实验台 lan_image.c 各自独立）
    list(APPEND MAIN_INCLUDE_DIRS "lan")
    # .incbin 资产搜索路径：gas 只搜 cwd + as -I（gcc 的 -I 仅进 cpp
    # 不达 as，2026-09-16 实测）——-Wa,-I,<dir> 拆成 as 的 -I <dir> 两参
    set_source_files_properties("app/data/embed_data.S" PROPERTIES
        COMPILE_OPTIONS "-Wa,-I,${CMAKE_CURRENT_SOURCE_DIR}/app/data")
endif()

idf_component_register(
    SRCS ${MAIN_SRCS}
    INCLUDE_DIRS ${MAIN_INCLUDE_DIRS}
    PRIV_REQUIRES epdiy esp_wifi esp_netif esp_event esp_http_server nvs_flash json spiffs esp_adc
)
