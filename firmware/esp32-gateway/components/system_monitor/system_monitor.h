/**
 * @file system_monitor.h
 * @author EnterWorldDoor
 * @brief 企业级系统监控模块：CPU/内存/任务/WDT/阈值告警/历史趋势
 *
 * 功能特性:
 *   - CPU 使用率实时监控（基于空闲任务运行时间比例）
 *   - 内存监控（堆空闲、最小值、碎片率、PSRAM）
 *   - 任务级监控（数量、单个任务栈使用率）
 *   - 阈值告警机制（CPU/内存超限自动触发回调）
 *   - 历史数据记录（可配置环形缓冲区深度）
 *   - FreeRTOS Mutex 线程安全
 *   - ESP-IDF Task Watchdog 集成
 */

#ifndef SYSTEM_MONITOR_H_
#define SYSTEM_MONITOR_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "global_error.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ==================== 配置常量 ==================== */

#define MONITOR_MAX_TASKS           16       /**< 最大监控任务数 */
#define MONITOR_HISTORY_DEPTH      60       /**< 历史数据环形缓冲区深度（默认60个采样点）*/
#define MONITOR_MAX_CALLBACKS      4        /**< 最大告警回调数 */
#define MONITOR_DEFAULT_INTERVAL   5000     /**< 默认采样间隔 (ms) */
#define MONITOR_TASK_STACK_SIZE    4096     /**< 监控任务栈大小（字节）*/
#define MONITOR_TASK_PRIORITY      5        /**< 监控任务优先级 */

/* ==================== 告警类型枚举 ==================== */

enum monitor_alarm_type {
    MONITOR_ALARM_CPU_HIGH = 0,     /**< CPU 使用率超过阈值 */
    MONITOR_ALARM_HEAP_LOW,        /**< 堆内存低于阈值 */
    MONITOR_ALARM_HEAP_MIN_LOW,    /**< 最小堆内存低于阈值 */
    MONITOR_ALARM_TASK_STACK_HIGH, /**< 单任务栈使用率超过阈值 */
    MONITOR_ALARM_COUNT            /**< 告警类型总数 */
};

/* ==================== 数据结构 ==================== */

/**
 * struct task_info - 单个任务的详细信息
 */
struct task_info {
    char name[configMAX_TASK_NAME_LEN]; /**< 任务名称 */
    uint32_t stack_high_water_mark;     /**< 栈高水位（剩余字节数）*/
    uint32_t stack_size;                /**< 栈总大小（字节）*/
    float stack_usage_percent;          /**< 栈使用率百分比 */
    UBaseType_t task_number;             /**< 任务编号 */
};

/**
 * struct memory_info - 内存详细信息
 */
struct memory_info {
    size_t free_heap;                   /**< SRAM 空闲堆大小（字节）*/
    size_t min_free_heap;               /**< 历史最小空闲堆（字节）*/
    size_t total_heap;                  /**< 堆总量（字节）*/
    float fragmentation_percent;        /**< 内存碎片率 (%) */
    size_t free_psram;                  /**< PSRAM 空闲大小（字节），0=无PSRAM */
    size_t min_free_psram;              /**< PSRAM 历史最小空闲 */
};

/**
 * struct cpu_info - CPU 信息
 */
struct cpu_info {
    float usage_percent;                /**< CPU 使用率 (0~100%) */
    uint32_t idle_ticks_total;          /**< 空闲任务累计 tick 数 */
    uint32_t total_ticks;               /**< 系统累计 tick 数 */
};

/**
 * struct system_status - 完整系统状态快照
 */
struct system_status {
    struct cpu_info cpu;                 /**< CPU 信息 */
    struct memory_info mem;             /**< 内存信息 */
    int task_count;                     /**< 当前运行的任务总数 */
    struct task_info tasks[MONITOR_MAX_TASKS]; /**< 各任务详情 */
    int task_info_count;                /**< 实际获取到的任务数 */
    uint64_t uptime_ms;                 /**< 系统运行时间（毫秒）*/
};

/**
 * struct monitor_history_entry - 单条历史记录
 */
struct monitor_history_entry {
    float cpu_usage;                    /**< CPU 使用率 */
    size_t free_heap;                   /**< 空闲堆 */
    uint32_t timestamp_s;               /**< 时间戳（秒）*/
};

/**
 * struct monitor_thresholds - 告警阈值配置
 */
struct monitor_thresholds {
    float cpu_warn_percent;             /**< CPU 警告阈值 (0~100)，0=禁用 */
    float cpu_critical_percent;        /**< CPU 严重阈值 (0~100) */
    size_t heap_warn_bytes;             /**< 堆内存警告阈值（字节），0=禁用 */
    size_t heap_critical_bytes;        /**< 堆内存严重阈值（字节）*/
    float stack_warn_percent;           /**< 任务栈警告阈值 (0~100)，0=禁用 */
    float stack_critical_percent;      /**< 任务栈严重阈值 (0~100) */
};

/**
 * struct monitor_config - 监控器配置
 */
struct monitor_config {
    uint32_t interval_ms;               /**< 采样间隔（毫秒）*/
    int history_depth;                  /**< 历史记录深度，0=不记录 */
    bool enable_wdt_feed;               /**< 是否在监控循环中喂看门狗 */
    struct monitor_thresholds thresholds; /**< 告警阈值 */
};

/* ==================== 回调函数类型 ==================== */

/**
 * monitor_alarm_callback_t - 阈值告警回调函数类型
 * @alarm_type: 告警类型 (enum monitor_alarm_type)
 * @current_value: 当前触发的值
 * @threshold_value: 阈值值
 * @user_data: 用户自定义上下文指针
 */
typedef void (*monitor_alarm_callback_t)(int alarm_type, float current_value,
                                         float threshold_value, void *user_data);

/* ==================== 生命周期管理 ==================== */

/**
 * system_monitor_init - 初始化系统监控模块
 * @interval_ms: 采样间隔（毫秒），0 表示仅手动查询模式（不启动后台任务）
 *
 * Return: APP_ERR_OK on success, negative error code on failure
 */
int system_monitor_init(uint32_t interval_ms);

/**
 * system_monitor_init_with_config - 使用完整配置初始化
 * @cfg: 配置结构体指针
 *
 * Return: APP_ERR_OK on success, negative error code on failure
 */
int system_monitor_init_with_config(const struct monitor_config *cfg);

/**
 * system_monitor_deinit - 反初始化监控模块，停止后台任务并释放资源
 *
 * Return: APP_ERR_OK on success, negative error code on failure
 */
int system_monitor_deinit(void);

/**
 * system_monitor_is_initialized - 查询是否已初始化
 *
 * Return: true 已初始化，false 未初始化
 */
bool system_monitor_is_initialized(void);

/* ==================== 状态查询（线程安全）==================== */

/**
 * system_monitor_get_status - 获取完整系统状态快照
 * @status: 输出状态结构体指针
 *
 * Return: APP_ERR_OK on success, APP_ERR_INVALID_PARAM if status is NULL
 */
int system_monitor_get_status(struct system_status *status);

/**
 * system_monitor_get_cpu - 获取 CPU 信息
 * @cpu: 输出 CPU 信息指针
 *
 * Return: APP_ERR_OK or error code
 */
int system_monitor_get_cpu(struct cpu_info *cpu);

/**
 * system_monitor_get_memory - 获取内存信息
 * @mem: 输出内存信息指针
 *
 * Return: APP_ERR_OK or error code
 */
int system_monitor_get_memory(struct memory_info *mem);

/**
 * system_monitor_get_task_count - 获取当前任务数量
 *
 * Return: 任务数量，未初始化返回 -1
 */
int system_monitor_get_task_count(void);

/**
 * system_monitor_get_task_info - 获取指定索引的任务信息
 * @index: 任务索引 (0 ~ task_count-1)
 * @info: 输出任务信息指针
 *
 * Return: APP_ERR_OK or error code
 */
int system_monitor_get_task_info(int index, struct task_info *info);

/**
 * system_monitor_get_uptime - 获取系统运行时间
 *
 * Return: 运行时间（毫秒）
 */
uint64_t system_monitor_get_uptime(void);

/* ==================== 日志输出 ==================== */

/**
 * system_monitor_dump - 将完整系统状态打印到日志
 */
void system_monitor_dump(void);

/**
 * system_monitor_dump_tasks - 打印各任务栈使用情况
 */
void system_monitor_dump_tasks(void);

/**
 * system_monitor_dump_memory - 打印详细内存信息
 */
void system_monitor_dump_memory(void);

/* ==================== 阈值与告警 ==================== */

/**
 * system_monitor_set_thresholds - 设置告警阈值
 * @thresholds: 新的阈值配置
 *
 * Return: APP_ERR_OK or error code
 */
int system_monitor_set_thresholds(const struct monitor_thresholds *thresholds);

/**
 * system_monitor_get_thresholds - 获取当前阈值配置
 * @thresholds: 输出阈值指针
 *
 * Return: APP_ERR_OK or error code
 */
int system_monitor_get_thresholds(struct monitor_thresholds *thresholds);

/**
 * system_monitor_register_alarm_callback - 注册告警回调
 * @callback: 回调函数
 * @user_data: 用户上下文（传递给回调）
 *
 * Return: APP_ERR_OK or error code
 */
int system_monitor_register_alarm_callback(monitor_alarm_callback_t callback,
                                          void *user_data);

/**
 * system_monitor_unregister_alarm_callback - 注销告警回调
 * @callback: 待注销的回调函数
 *
 * Return: APP_ERR_OK or error code
 */
int system_monitor_unregister_alarm_callback(monitor_alarm_callback_t callback);

/* ==================== 历史数据 ==================== */

/**
 * system_monitor_get_history_depth - 获取已记录的历史条目数
 *
 * Return: 条目数量
 */
int system_monitor_get_history_depth(void);

/**
 * system_monitor_get_history_entry - 获取指定位置的历史记录
 * @index: 索引 (0=最新, depth-1=最旧)
 * @entry: 输出条目指针
 *
 * Return: APP_ERR_OK or error code
 */
int system_monitor_get_history_entry(int index,
                                     struct monitor_history_entry *entry);

/**
 * system_monitor_clear_history - 清空历史记录
 */
void system_monitor_clear_history(void);

#endif /* SYSTEM_MONITOR_H_ */