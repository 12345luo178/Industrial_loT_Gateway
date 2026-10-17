# ============================================================================
#  ESP-IDF 工程入口 —— gateway_bridge（STM32 <-> 云端 桥接固件）
#
#  目录即工程：`esp32/` 本身就是 ESP-IDF 工程根，
#  不复制任何源码 —— main/ 组件直接引用 `../src/`（桥接逻辑）
#  与 `../../src/gw_frame.c`（帧协议，与 STM32 共用同一份文件）。
#  这样 PC 单元测试与固件编译测的是**同一份源码**。
#
#  编译（先安装 ESP-IDF v5.x 并 export.bat 导入环境）：
#      idf.py set-target esp32
#      idf.py build
#  烧录与监视：
#      idf.py -p COMx flash monitor
#
#  ⚠️ Wi-Fi/MQTT 凭据在 main.c 顶部宏里（GW_WIFI_SSID 等），烧录前改掉。
#     这是文档 §4 记录的已知简化；生产做法是经 GW_FT_WIFI_CONFIG 帧写入 NVS。
# ============================================================================
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gateway_bridge)
