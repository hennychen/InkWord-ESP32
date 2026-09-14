# main 组件注册（espidf 框架）
# 目录职责（方案 §2.1）：
#   main.c                 入口（probe env 走 bring-up 序列，否则安全初始化）
#   driver/                板级 GPIO 镜像 + ES108FC1C1-RHY 面板安全初始化
#   config/                屏幕/总线/VCOM 参数（保守起步值集中管理）
#   hal/                   显示抽象接口骨架（Phase 2 实现，业务不直接调 epdiy）
#   test/                  bring-up 八步序列（BIGSCREEN_PROBE 门控完整路径）
#   app/                   业务逻辑移植目标目录（Phase 2+ 创建，当前未建）
# demo/：卖家 ED060KD1 demo 复刻（BIGSCREEN_SELLER_DEMO 构建；图片头
# 仅 demo_seller.c 单翻译单元 include，非 static const 避免重复链接）
# lan/：LAN 图片上传显示（BIGSCREEN_LAN 构建，适配里程碑 M1）
idf_component_register(
    SRCS "main.c" "driver/panel_es108fc.c" "test/test_basic.c" "test/demo/demo_seller.c"
         "lan/lan_image.c"
    INCLUDE_DIRS "." "driver" "config" "test"
    PRIV_REQUIRES epdiy esp_wifi esp_netif esp_event esp_http_server nvs_flash
)
