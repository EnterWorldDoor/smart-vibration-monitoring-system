# main 组件：EdgeVib ESP32-S3 主程序入口
#
# 依赖说明:
#   REQUIRES = 公共依赖
#     - adxl345       : ADXL355 加速度传感器驱动
#     - sensor_service : 传感器数据采集服务
#     - time_sync     : 时间同步模块
#     - log_system    : 日志系统
#     - mqtt          : MQTT 通信
#     - freertos      : FreeRTOS
#     - esp_netif     : 网络接口
#     - esp_event     : 事件系统
#   PRIV_REQUIRES = 私有依赖
#     - unity         : 单元测试框架 (如需测试请创建 *_test.c 文件)
#
idf_component_register(
    SRCS "esp32-gateway.c"
    INCLUDE_DIRS "."
    REQUIRES adxl345 sensor_service time_sync log_system mqtt freertos esp_netif esp_event
    PRIV_REQUIRES unity
)
