/**
 * @file time_sync.h
 * @author EnterWorldDoor
 * @brief 系统时间同步模块 (基于 SNTP 于本地高精度计时器)
 */


#ifndef TIME_SYNC_H
#define TIME_SYNC_H

#include <stdint.h>
#include <stdbool.h>
#include "global_error.h"

/**
 * time_sync_init - 初始化时间同步模块
 * @use_sntp: 是否使用 SNTP 进行网络时间同步(若为 false, 仅使用本地 ESP 计时器进行时间维护)
 * @timezone: 时区字符串 (如 "CST-8" 表示中国标准时间, UTC+8)
 * 
 * Return: 0 on success, negative error code on failure
 */
int time_sync_init(bool use_sntp, const char *timezone);

/**
 * time_sync_get_timestamp_us - 获取当前高精度时间戳 (单位: 微秒)
 * 
 * 返回值基于 ESP32 内部计时器 (esp_timer_get_time),
 * 若 SNTP 已同步, 则绝对时间戳 (从1970-01-01 00:00:00 UTC 起的微秒数), 
 * 否则为相对时间戳 (从模块初始化时起的微秒数)
 * 
 * Return: 当前时间戳 (单位: 微秒)
 */
int64_t time_sync_get_timestamp_us(void);

/**
 * time_sync_get_timestamp_ms - 获取当前高精度时间戳 (单位: 毫秒)
 */
static inline int64_t time_sync_get_timestamp_ms(void)
{
    return time_sync_get_timestamp_us() / 1000;
}

/**
 * time_sync_wait_sync - 等待 SNTP 时间同步完成(阻塞)
 * @timeout_ms: 等待超时时间 (单位: 毫秒), 若为 0 则无限等待
 * 
 * Return: 0 if synced, -APP_ERR_TIMEOUT if timeout, negative error code on failure
 */
int time_sync_wait_sync(int timeout_ms);

#endif /* TIME_SYNC_H */