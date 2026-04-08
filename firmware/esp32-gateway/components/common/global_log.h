/**
 * @file log.h
 * @author EnterWorldDoor 
 * @brief 日志系统宏封装 (支持等级控制,颜色输出)
 */

#ifndef COMMON_GLOBAL_LOG_H
#define COMMON_GLOBAL_LOG_H

#include "esp_log.h"

#define LOG_TAG "EDGEVIB"

#define LOG_INFO(fmt, ...) ESP_LOGI(LOG_TAG, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) ESP_LOGW(LOG_TAG, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) ESP_LOGE(LOG_TAG, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) ESP_LOGD(LOG_TAG, fmt, ##__VA_ARGS__)
#define LOG_VERBOSE(fmt, ...) ESP_LOGV(LOG_TAG, fmt, ##__VA_ARGS__)

#endif /* COMMON_GLOBAL_LOG_H */