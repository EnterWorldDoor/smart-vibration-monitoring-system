#include <stdio.h>
#include "log_system.h"
#include "time_sync.h"
#include "sensor_service.h"
#include "global_error.h"
#include "esp_netif.h"
#include "esp_event.h"

#ifdef CONFIG_UNITY_ENABLE_BACKTRACE_ON_FAIL
#include "unity.h"
#endif

void app_main(void)
{
#ifdef CONFIG_UNITY_ENABLE_BACKTRACE_ON_FAIL
    /* ==================== Unity 测试模式 ==================== */
    /* 当 sdkconfig 中开启 UNITY 选项时，app_main 直接运行全部单元测试 */
    printf("\n========================================\n");
    printf("   EdgeVib Unity Unit Test Runner\n");
    printf("========================================\n\n");
    unity_run_all_tests();
    printf("\n========================================\n");
    printf("   All tests completed!\n");
    printf("========================================\n");
#else
    /* ==================== 正常生产模式 ==================== */
    /* 初始化日志系统 */
    log_config_t log_config = {
        .level = LOG_LEVEL_INFO,
        .outputs = LOG_OUTPUT_UART,
        .ringbuf_size = 4096
    };
    int ret = log_system_init_with_config(&log_config);
    if (ret != APP_ERR_OK) {
        printf("Failed to initialize log system: %d\n", ret);
        return;
    }

    LOG_INFO("MAIN", "ESP32 Gateway started");

    /* 初始化网络基础设施 (SNTP/MQTT/WiFi 的前置依赖) */
    ret = esp_netif_init();
    if (ret != ESP_OK) {
        LOG_ERROR("MAIN", "esp_netif_init failed: %d", ret);
    }
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        LOG_ERROR("MAIN", "esp_event_loop_create_default failed: %d", ret);
    }

    /* 初始化时间同步 */
    ret = time_sync_init(true, "CST-8");
    if (ret != APP_ERR_OK) {
        LOG_ERROR("MAIN", "Failed to initialize time sync: %d", ret);
    } else {
        /* 等待 SNTP 同步 */
        ret = time_sync_wait_sync(10000);
        if (ret != APP_ERR_OK) {
            LOG_WARN("MAIN", "SNTP sync timeout, using local time");
        }
    }
    
    /* 初始化传感器服务 */
    ret = sensor_service_init(100, 1000);
    if (ret != APP_ERR_OK) {
        LOG_ERROR("MAIN", "Failed to initialize sensor service: %d", ret);
    } else {
        /* 启动传感器服务 */
        ret = sensor_service_start();
        if (ret != APP_ERR_OK) {
            LOG_ERROR("MAIN", "Failed to start sensor service: %d", ret);
        } else {
            LOG_INFO("MAIN", "Sensor service started");
        }
    }
    
    LOG_INFO("MAIN", "Initialization completed");
#endif /* CONFIG_UNITY_ENABLE_BACKTRACE_ON_FAIL */
}
