/**
 * @file time_sync.h
 * @author EnterWorldDoor
 * @brief 企业级时间同步模块：SNTP 网络同步 + 本地高精度计时器 + 自动周期同步
 *
 * 功能特性:
 *   - 基于 ESP-IDF SNTP 的网络时间协议 (NTP/SNTP) 同步
 *   - 本地高精度微秒级时间戳 (基于 esp_timer_get_time)
 *   - 自动周期性时间同步（可配置间隔）
 *   - 多 NTP 服务器支持与故障切换
 *   - 时间同步状态查询与回调通知
 *   - 时区管理与本地时间转换
 *   - FreeRTOS Mutex 线程安全
 *   - 与 config_manager 集成获取配置参数
 */

#ifndef TIME_SYNC_H
#define TIME_SYNC_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "global_error.h"

/* ==================== 配置常量 ==================== */

#define TIME_SYNC_MAX_SERVERS        4       /**< 最大 NTP 服务器数量 */
#define TIME_SYNC_MAX_TIMEZONE_LEN    16      /**< 最大时区字符串长度 */
#define TIME_SYNC_MAX_SERVER_LEN      64      /**< 最大服务器地址长度 */
#define TIME_SYNC_DEFAULT_TIMEOUT_MS  10000   /**< 默认 SNTP 超时 (ms) */
#define TIME_SYNC_DEFAULT_RETRY_MS    5000    /**< 默认重试间隔 (ms) */
#define TIME_SYNC_DEFAULT_MAX_RETRIES 3       /**< 默认最大重试次数 */
#define TIME_SYNC_DEFAULT_INTERVAL_S 3600    /**< 默认自动同步间隔 (秒)*/
#define TIME_SYNC_TASK_STACK_SIZE     2048    /**< 同步任务栈大小（字节）*/
#define TIME_SYNC_TASK_PRIORITY      5       /**< 同步任务优先级 */
#define TIME_SYNC_MAX_CALLBACKS       4       /**< 最大回调注册数 */
#define TIME_SYNC_MONITOR_STACK_SIZE 2048    /**< SNTP 监控任务栈大小（字节）*/

/* ==================== 同步状态枚举 ==================== */

enum sync_status {
    SYNC_STATUS_IDLE = 0,            /**< 空闲，未初始化或未启用 SNTP */
    SYNC_STATUS_INITIALIZED,         /**< 已初始化，等待首次同步 */
    SYNC_STATUS_SYNCHRONIZING,       /**< 正在同步中 */
    SYNC_STATUS_SYNCED,              /**< 已成功同步 */
    SYNC_STATUS_FAILED,              /**< 同步失败（超过重试次数）*/
    SYNC_STATUS_COUNT                /**< 状态总数 */
};

/* ==================== 时间信息结构体 ==================== */

/**
 * struct time_info - 完整的时间信息快照
 */
struct time_info {
    int64_t timestamp_us;            /**< 绝对时间戳（微秒，从 1970-01-01 UTC）*/
    int64_t timestamp_ms;            /**< 绝对时间戳（毫秒）*/
    uint32_t uptime_s;               /**< 系统运行时间（秒）*/
    struct tm local_time;            /**< 本地时间（已转换时区）*/
    struct tm utc_time;              /**< UTC 时间 */
    enum sync_status status;         /**< 当前同步状态 */
    bool is_synchronized;           /**< 是否已完成至少一次成功同步 */
    uint32_t last_sync_epoch;        /**< 上次成功同步的 Unix 时间戳 */
    int32_t drift_us;                /**< 与上次读取的漂移量（微秒），用于监控 */
};

/**
 * struct sntp_server_config - NTP 服务器配置
 */
struct sntp_server_config {
    char server[TIME_SYNC_MAX_SERVER_LEN]; /**< 服务器地址 */
    bool enabled;                       /**< 是否启用此服务器 */
};

/* ==================== 回调函数类型 ==================== */

/**
 * time_sync_callback_t - 时间同步完成回调
 * @status: 同步结果状态
 * @timestamp_us: 同步后的绝对时间戳（微秒）
 * @user_data: 用户自定义上下文指针
 */
typedef void (*time_sync_callback_t)(enum sync_status status,
                                    int64_t timestamp_us,
                                    void *user_data);

/* ==================== 生命周期管理 ==================== */

/**
 * time_sync_init - 初始化时间同步模块
 * @use_sntp: 是否使用 SNTP 进行网络时间同步
 * @timezone: 时区字符串（如 "CST-8" 表示中国标准时间, UTC+8）
 *             若为 NULL 则使用 config_manager 中配置的时区
 *
 * Context: 必须在 FreeRTOS 调度启动后调用（需创建 EventGroup/Task）
 * Return: APP_ERR_OK on success, negative error code on failure
 */
int time_sync_init(bool use_sntp, const char *timezone);

/**
 * time_sync_deinit - 反初始化时间同步模块
 *                  停止自动同步任务、释放资源
 *
 * Return: APP_ERR_OK on success, negative error code on failure
 */
int time_sync_deinit(void);

/**
 * time_sync_is_initialized - 查询是否已初始化
 *
 * Return: true 已初始化，false 未初始化
 */
bool time_sync_is_initialized(void);

/* ==================== 时间戳获取（线程安全）==================== */

/**
 * time_sync_get_timestamp_us - 获取当前高精度时间戳（微秒）
 *
 * 返回值:
 *   - 若 SNTP 已同步：绝对时间戳（从 1970-01-01 00:00:00 UTC 起的微秒数）
 *   - 若仅本地模式：相对时间戳（从模块初始化时起的微秒数）
 *
 * Return: 当前时间戳（微秒）
 */
int64_t time_sync_get_timestamp_us(void);

/**
 * time_sync_get_timestamp_ms - 获取当前时间戳（毫秒）
 */
static inline int64_t time_sync_get_timestamp_ms(void)
{
    return time_sync_get_timestamp_us() / 1000;
}

/**
 * time_sync_get_time_info - 获取完整时间信息快照（含本地/UTC时间）
 * @info: 输出时间信息结构体指针
 *
 * Return: APP_ERR_OK or error code
 */
int time_sync_get_time_info(struct time_info *info);

/* ==================== 同步控制 ==================== */

/**
 * time_sync_wait_sync - 等待 SNTP 时间同步完成（阻塞）
 * @timeout_ms: 等待超时时间（毫秒），0 表示无限等待
 *
 * Return: APP_ERR_OK if synced, APP_ERR_TIME_SNTPI_SYNC_TIMEOUT if timeout
 */
int time_sync_wait_sync(int timeout_ms);

/**
 * time_sync_force_sync - 强制触发一次立即同步（异步）
 *
 * Return: APP_ERR_OK on success, negative error code on failure
 */
int time_sync_force_sync(void);

/**
 * time_sync_get_status - 获取当前同步状态
 *
 * Return: 当前同步状态枚举值
 */
enum sync_status time_sync_get_status(void);

/**
 * time_sync_is_synchronized - 查询是否已完成至少一次成功同步
 *
 * Return: true 已同步，false 未同步
 */
bool time_sync_is_synchronized(void);

/* ==================== NTP 服务器管理 ==================== */

/**
 * time_sync_set_servers - 设置 NTP 服务器列表
 * @servers: 服务器配置数组
 * @count: 服务器数量（最多 TIME_SYNC_MAX_SERVERS 个）
 *
 * Return: APP_ERR_OK or error code
 */
int time_sync_set_servers(const struct sntp_server_config *servers, int count);

/**
 * time_sync_get_servers - 获取当前 NTP 服务器配置
 * @servers: 输出服务器配置数组
 * @max_count: 数组容量
 * @out_count: 输出实际服务器数量
 *
 * Return: APP_ERR_OK or error code
 */
int time_sync_get_servers(struct sntp_server_config *servers,
                         int max_count, int *out_count);

/* ==================== 回调注册 ==================== */

/**
 * time_sync_register_callback - 注册同步完成回调
 * @callback: 回调函数
 * @user_data: 用户上下文（传递给回调）
 *
 * Return: APP_ERR_OK or error code
 */
int time_sync_register_callback(time_sync_callback_t callback,
                                void *user_data);

/**
 * time_sync_unregister_callback - 注销同步回调
 * @callback: 待注销的回调函数
 *
 * Return: APP_ERR_OK or error code
 */
int time_sync_unregister_callback(time_sync_callback_t callback);

#endif /* TIME_SYNC_H */