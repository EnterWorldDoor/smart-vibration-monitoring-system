# main 组件：EdgeVib ESP32-S3 主程序入口
#
# 依赖说明:
#   REQUIRES = 公共依赖
#     - esp_wifi      : WiFi STA模式 (连接手机热点) [新增]
#     - nvs_flash     : NVS 非易失性存储 (配置持久化) [新增]
#     - adxl345       : ADXL355 加速度传感器驱动
#     - sensor_service : 传感器数据采集服务
#     - time_sync     : 时间同步模块
#     - log_system    : 日志系统
#     - mqtt_app      : MQTT 通信 (重命名避免与 ESP-IDF 内置 mqtt 冲突)
#     - freertos      : FreeRTOS
#     - esp_netif     : 网络接口
#     - esp_event     : 事件系统
#   PRIV_REQUIRES = 私有依赖
#     - unity         : 单元测试框架 (如需测试请创建 *_test.c 文件)
#
idf_component_register(
    SRCS "esp32-gateway.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi nvs_flash adxl345 sensor_service time_sync log_system mqtt_app freertos esp_netif esp_event
    # PRIV_REQUIRES unity  ← 🔴 已移除! 禁止编译所有*_test.c文件,避免app_main()冲突
)
