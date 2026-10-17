# ============================================================================
#  main 组件 —— 桥接固件本体
#
#  源码不复制、直接引用仓库里的原始位置：
#    ../src/main.c            硬件相关层（UART/WiFi/MQTT/任务）
#    ../src/esp32_json.c      JSON 封装/解析（154 项 PC 测试）
#    ../src/esp32_bridge.c    退避/指令确认/积压（126 项 PC 测试）
#    ../../src/gw_frame.c     帧协议（82 项 PC 测试，与 STM32 共用）
#
#  REQUIRES 说明：
#    driver     uart/gpio 驱动
#    esp_wifi   Wi-Fi STA
#    esp_event  事件循环（WIFI_EVENT/IP_EVENT/MQTT 事件）
#    esp_netif  esp_netif_init() / 默认 STA netif
#    nvs_flash  NVS（Wi-Fi 驱动依赖）
#    mqtt       esp-mqtt 客户端
#    esp_timer  esp_timer_get_time()
#    log        ESP_LOGx
#    lwip       esp_sntp（SNTP 在 lwip 组件里）+ gettimeofday
# ============================================================================
idf_component_register(
    SRCS "../src/main.c"
         "../src/esp32_json.c"
         "../src/esp32_bridge.c"
         "../../src/gw_frame.c"
    INCLUDE_DIRS "../include" "../../inc"
    REQUIRES driver esp_wifi esp_event esp_netif nvs_flash mqtt esp_timer log lwip
)
