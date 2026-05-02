/**
 * @file system_log.h
 * @brief 企业级日志系统（Linux内核风格、多等级、可配置）
 *
 * 适用场景:
 *   - 系统运行状态监控
 *   - 调试信息输出
 *   - 错误追踪与诊断
 *
 * 设计特点:
 *   - Linux 内核风格的日志宏接口
 *   - 4 个日志等级: DEBUG < INFO < WARN < ERROR
 *   - 编译期和运行期双重级别过滤
 *   - 支持时间戳和模块标签
 *   - 通过回调函数实现输出目标可定制化
 *   - 纯软件实现，无硬件依赖
 */

#ifndef __SYSTEM_LOG_H
#define __SYSTEM_LOG_H

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include "global_error/global_error.h"

/* ==================== 日志等级定义 ==================== */

/**
 * enum log_level - 日志等级枚举
 */
enum log_level {
        LOG_LEVEL_DEBUG = 0,            /**< 调试信息，开发阶段使用 */
        LOG_LEVEL_INFO  = 1,            /**< 正常运行信息 */
        LOG_LEVEL_WARN  = 2,            /**< 警告，功能可执行但有问题 */
        LOG_LEVEL_ERROR = 3,            /**< 错误，功能无法执行 */
        LOG_LEVEL_NONE  = 4,            /**< 关闭所有日志输出 */
};

/* ==================== 日志配置 ==================== */

/*
 * 默认日志标签，可通过 log_set_tag() 修改。
 * 建议每个模块使用不同的标签以便过滤。
 */
#define LOG_DEFAULT_TAG         "STM32"

/*
 * 默认日志缓冲区大小（单条日志最大长度）。
 * 包含时间戳、标签、格式化内容等所有信息。
 */
#define LOG_MAX_LINE_SIZE       256

/*
 * 默认日志队列深度（异步模式下的缓存条数）。
 * 设为 0 表示使用同步模式（直接输出）。
 */
#define LOG_QUEUE_DEPTH          64

/* ==================== 输出回调类型 ==================== */

/**
 * typedef log_output_func_t - 日志输出函数指针类型
 * @data: 格式化后的日志字符串
 * @len: 字符串长度 (字节)
 *
 * 实现者负责将日志输出到具体设备（UART、USB、文件等）。
 */
typedef void (*log_output_func_t)(const char *data, uint16_t len);

/* ==================== 日志系统结构体 ==================== */

/**
 * struct log_config - 日志系统配置
 * @level: 运行时最低输出等级
 * @tag: 默认日志标签
 * @output: 输出回调函数
 * @enable_timestamp: 是否启用时间戳
 * @async_mode: 是否使用异步模式
 */
struct log_config {
        enum log_level level;
        const char *tag;
        log_output_func_t output;
        bool enable_timestamp;
        bool async_mode;
};

/**
 * struct log_stats - 日志统计信息
 * @total_lines: 总输出行数
 * @dropped_lines: 因缓冲区满丢弃的行数
 * @bytes_written: 总写入字节数
 * @level_counts[4]: 各等级的输出次数
 */
struct log_stats {
        uint32_t total_lines;
        uint32_t dropped_lines;
        uint32_t bytes_written;
        uint32_t level_counts[4];
};

/* ==================== 初始化 API ==================== */

/**
 * log_init - 初始化日志系统
 * @config: 日志配置指针 (NULL 使用默认配置)
 *
 * 必须在系统启动后调用一次。
 * 默认配置: INFO 等级, 标签 "STM32", 无输出回调(需设置)
 *
 * Return: ERR_OK 成功, 负数错误码
 */
int log_init(const struct log_config *config);

/**
 * log_deinit - 反初始化日志系统
 */
void log_deinit(void);

/**
 * log_is_initialized - 查询是否已初始化
 * Return: true 已初始化, false 未初始化
 */
bool log_is_initialized(void);

/* ==================== 配置 API ==================== */

/**
 * log_set_level - 设置运行时日志等级
 * @level: 新的日志等级
 *
 * 只有 >= 该等级的日志才会被输出。
 */
void log_set_level(enum log_level level);

/**
 * log_get_level - 获取当前日志等级
 * Return: 当前日志等级
 */
enum log_level log_get_level(void);

/**
 * log_set_tag - 设置默认日志标签
 * @tag: 新的标签字符串
 */
void log_set_tag(const char *tag);

/**
 * log_set_output - 设置输出回调函数
 * @output: 输出函数指针 (NULL 禁用输出)
 */
void log_set_output(log_output_func_t output);

/**
 * log_enable_timestamp - 启用/禁用时间戳
 * @enable: true 启用, false 禁用
 */
void log_enable_timestamp(bool enable);

/* ==================== 核心 API ==================== */

/**
 * log_write - 写入一条日志（核心函数）
 * @level: 日志等级
 * @tag: 模块标签 (NULL 使用默认标签)
 * @fmt: 格式化字符串
 * @...: 可变参数列表
 *
 * 这是所有日志宏的底层实现。
 * 一般不直接调用，建议使用下方的便捷宏。
 */
void log_write(enum log_level level, const char *tag,
               const char *fmt, ...);

/* ==================== 便捷宏（Linux内核风格） ==================== */

/*
 * 使用示例:
 *   pr_info("system started\n");
 *   pr_err("sensor init failed: %d\n", ret);
 *   pr_debug("adc value: %u\n", adc_val);
 */

/**
 * pr_debug - 输出 DEBUG 级别日志
 * 仅在 DEBUG 或更低等级时编译进代码
 */
#if defined(LOG_ENABLE_DEBUG) || defined(DEBUG)
#define pr_debug(fmt, ...) \
        log_write(LOG_LEVEL_DEBUG, NULL, fmt, ##__VA_ARGS__)
#else
#define pr_debug(fmt, ...) ((void)0)
#endif

/**
 * pr_info - 输出 INFO 级别日志
 */
#define pr_info(fmt, ...) \
        log_write(LOG_LEVEL_INFO, NULL, fmt, ##__VA_ARGS__)

/**
 * pr_warn - 输出 WARN 级别日志
 */
#define pr_warn(fmt, ...) \
        log_write(LOG_LEVEL_WARN, NULL, fmt, ##__VA_ARGS__)

/**
 * pr_error - 输出 ERROR 级别日志
 */
#define pr_error(fmt, ...) \
        log_write(LOG_LEVEL_ERROR, NULL, fmt, ##__VA_ARGS__)

/* ==================== 带标签的便捷宏 ==================== */

/**
 * pr_debugWithTag - 带自定义标签的 DEBUG 日志
 * @tag: 模块标签
 */
#if defined(LOG_ENABLE_DEBUG) || defined(DEBUG)
#define pr_debug_with_tag(tag, fmt, ...) \
        log_write(LOG_LEVEL_DEBUG, tag, fmt, ##__VA_ARGS__)
#else
#define pr_debug_with_tag(tag, fmt, ...) ((void)0)
#endif

/**
 * pr_info_with_tag - 带自定义标签的 INFO 日志
 */
#define pr_info_with_tag(tag, fmt, ...) \
        log_write(LOG_LEVEL_INFO, tag, fmt, ##__VA_ARGS__)

/**
 * pr_warn_with_tag - 带自定义标签的 WARN 日志
 */
#define pr_warn_with_tag(tag, fmt, ...) \
        log_write(LOG_LEVEL_WARN, tag, fmt, ##__VA_ARGS__)

/**
 * pr_error_with_tag - 带自定义标签的 ERROR 日志
 */
#define pr_error_with_tag(tag, fmt, ...) \
        log_write(LOG_LEVEL_ERROR, tag, fmt, ##__VA_ARGS__)

/* ==================== 统计 API ==================== */

/**
 * log_get_stats - 获取日志统计快照
 * @stats: 输出统计结构体
 * Return: ERR_OK or error code
 */
int log_get_stats(struct log_stats *stats);

/**
 * log_reset_stats - 重置统计计数器
 */
void log_reset_stats(void);

#endif /* __SYSTEM_LOG_H */
