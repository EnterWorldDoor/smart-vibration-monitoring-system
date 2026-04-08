/**
 * @file time_sync.c
 * @author EnterWorldDoor
 * @brief 时间同步实现: 基于 SNTP 和 ESP32 内部高精度计时器(esp_timer_get_time)
 */

 #include "time_sync.h"
 #include "global_error.h"
 #include "log_system.h"
 #include "esp_timer.h"
 #include "esp_sntp.h"
 #include "freertos/FreeRTOS.h"
 #include "freertos/event_groups.h"
 #include <string.h>
 #include <time.h>

 #define SNTP_SERVER1 "pool.ntp.org"
 #define SNTP_SERVER2 "time.google.com"

 static EventGroupHandle_t s_sync_event = NULL;
 static const int SNTP_SYNC_BIT = BIT0;
 static bool s_use_sntp = false;
 static int64_t s_boot_time_us = 0; /* 记录系统启动时的绝对时间(若 SNTP 已同步) */

 /* SNTP 同步回调函数 */
 static void sntp_sync_cb(struct timeval *tv)
 {
    LOG_INFO("TIME", "SNTP synchronized, time: %s", ctime(&tv->tv_sec));
    /* 记录系统启动时的绝对时间偏移 */
    int64_t now_us = esp_timer_get_time();
    s_boot_time_us = (int64_t)tv->tv_sec * 1000000LL + tv->tv_usec - now_us;
    if (s_sync_event) {
        xEventGroupSetBits(s_sync_event, SNTP_SYNC_BIT);
    }
 }

 int time_sync_init(bool use_sntp, const char *timezone)
 {
    s_use_sntp = use_sntp;
    s_boot_time_us = 0;

    if (use_sntp) {
        s_sync_event = xEventGroupCreate();
        if (!s_sync_event) {
            LOG_ERROR("TIME", "Failed to create event group for SNTP sync");
            return APP_ERR_NO_MEM;
        }
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, SNTP_SERVER1);
        esp_sntp_setservername(1, SNTP_SERVER2);
        esp_sntp_set_time_sync_notification_cb(sntp_sync_cb);
        esp_sntp_init();
        LOG_INFO("TIME", "SNTP initialized, waiting for synchronization...");
    } else {
        LOG_INFO("TIME", "Using local timer only, no SNTP synchronization");
    }

    /* 设置时区 */
    if (timezone) {
        setenv("TZ", timezone, 1);
        tzset();
        LOG_INFO("TIME", "Timezone set to: %s", timezone);
    }

    return APP_ERR_OK;
 }

 int64_t time_sync_get_timestamp_us(void)
 {
    int64_t now_us = esp_timer_get_time();
    if (s_use_sntp && s_boot_time_us != 0) {
        /* 返回绝对时间戳 */
        return s_boot_time_us + now_us;
    } else {
        /* 返回相对时间戳(从系统启动开始) */
        return now_us;
    }
 }

 int time_sync_wait_sync(int timeout_ms)
 {
    if (!s_use_sntp) {
        LOG_WARN("TIME", "SNTP synchronization is disabled, returning immediately");
        return APP_ERR_OK;
    }
    if (!s_sync_event) {
        LOG_ERROR("TIME", "SNTP sync event group not initialized");
        return APP_ERR_GENERAL;
    }
    TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_sync_event, SNTP_SYNC_BIT, 
        pdFALSE, pdTRUE, ticks);
    if (bits & SNTP_SYNC_BIT) {
        return APP_ERR_OK; /* 已同步 */
    } else {
        return APP_ERR_TIMEOUT; /* 超时 */
    }
 }