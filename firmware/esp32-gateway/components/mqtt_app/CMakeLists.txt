# MQTT 通信模块 (基于 ESP-IDF esp-mqtt 组件)
# 功能: TLS加密 + 离线缓存 + 批量发布 + 动态配置更新
idf_component_register(
    SRCS "mqtt.c"
    INCLUDE_DIRS "."
    REQUIRES
        mqtt          # ESP-IDF 内置 MQTT 客户端组件 (提供 mqtt_client.h)
        common        # global_error.h + ringbuf.h
        config_manager # 配置管理 (动态更新)
        sensor_service # 传感器服务 (analysis_result)
        driver        # GPIO/WiFi 驱动
        freertos      # FreeRTOS 任务/信号量
        esp_wifi      # WiFi 配置 (esp_wifi.h)
        mbedtls       # TLS 加密支持 (mbedtls/aes.h, mbedtls/base64.h)
    PRIV_REQUIRES
        log_system    # 日志系统 (内部使用)
)
