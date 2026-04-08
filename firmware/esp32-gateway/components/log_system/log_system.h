/**
 * @file log_system.h
 * @author EnterWorldDoor
 * @brief 高级日志系统: 支持等级过滤、环形缓存、多输出(UART/MQTT/文件)
 */

#ifndef LOG_SYSTEM_H_
#define LOG_SYSTEM_H_

#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdbool.h>

/* 日志系统 */
typedef enum {
   LOG_LEVEL_ERROR = 0,
   LOG_LEVEL_WARN,
   LOG_LEVEL_INFO,
   LOG_LEVEL_DEBUG,
   LOG_LEVEL_VERBOSE
} log_level_t;

/* 日志输出目标  - 后续模块的日志输出可在此扩展，只要在log_system.c文件添加输出函数 */
typedef enum {
   LOG_OUTPUT_UART = 1 << 0,
   LOG_OUTPUT_RINGBUF = 1 << 1,
   LOG_OUTPUT_MQTT = 1 << 2,
   LOG_OUTPUT_FILE = 1 << 3,
   LOG_OUTPUT_REMOTE = 1 << 4,
} log_output_t;

/* 日志文件轮转策略 */
typedef enum {
   LOG_ROTATION_NONE = 0,
   LOG_ROTATION_SIZE,
   LOG_ROTATION_TIME,
   LOG_ROTATION_BOTH
} log_rotation_t;

/* 日志过滤规则结构体 */
typedef struct {
   char tag_pattern[64];     /* 标签匹配模式，支持通配符 */
   log_level_t min_level;    /* 最小日志等级 */
   log_level_t max_level;    /* 最大日志等级 */
   uint32_t enabled_outputs; /* 启用的输出目标 */
   bool enabled;             /* 规则是否启用 */
} log_filter_rule_t;

/* 日志配置结构体 */
typedef struct {
   log_level_t level;
   uint32_t outputs;
   size_t ringbuf_size;

   /* 文件系统配置 */
   char log_file_path[128];
   size_t max_file_size;
   int max_file_count;
   log_rotation_t rotation_policy;
   int rotation_time_hours;

   /* MQTT配置 */
   char mqtt_broker[64];
   int mqtt_port;
   char mqtt_topic[64];
   int mqtt_qos;
   bool mqtt_enabled;
   char mqtt_username[32];
   char mqtt_password[32];

   /* 远程配置 */
   char remote_url[128];
   int upload_interval_sec;
   bool remote_enabled;

   /* 性能配置 */
   bool async_mode;
   int queue_size;
   int batch_size;
   int flush_interval_ms;

   /* 安全配置 */
   bool encryption_enabled;
   char encryption_key[32];

   /* 压缩配置 */
   bool compression_enabled;
   int compression_level;

   /* 过滤规则配置 */
   #define MAX_FILTER_RULES 10
   log_filter_rule_t filter_rules[MAX_FILTER_RULES];
   int filter_rule_count;

} log_config_t;

/* 错误回调函数类型 */
typedef void (*log_error_callback_t)(int error_code, const char *error_msg);

/* 可插拔日志输出回调，用于测试替身或自定义企业级Sink */
typedef int (*log_sink_callback_t)(const char *message, void *user_ctx);

/**
 * log_system_init - 初始化日志系统（简化版）
 * @level: 全局日志等级 (低于此等级的日志不回输出)
 * @outputs: 输出目标掩码 (LOG_OUTPUT_* 组合)
 * @ringbuf_size: 环形缓冲区大小 (字节), 若为0则不启用环形缓冲
 *
 * Return: 0 on success, negitive error code
 */
int log_system_init(log_level_t level, uint32_t outputs, size_t ringbuf_size);

/**
 * log_system_init_with_config - 使用配置结构体初始化日志系统（高级版）
 * @config: 日志配置结构体指针
 *
 * Return: 0 on success, negative error code on failure
 */
int log_system_init_with_config(const log_config_t *config);

/**
 * log_set_error_callback - 设置错误回调函数
 * @callback: 错误回调函数指针
 */
void log_set_error_callback(log_error_callback_t callback);

/**
 * log_register_custom_sink - 注册自定义输出Sink（测试/扩展用）
 * @sink: 输出回调，传入完整格式化后的日志行
 * @user_ctx: 用户上下文指针
 *
 * Return: 0 on success, negative error code on failure
 */
int log_register_custom_sink(log_sink_callback_t sink, void *user_ctx);

/**
 * log_unregister_custom_sink - 注销自定义输出Sink
 */
void log_unregister_custom_sink(void);

/**
 * log_get_config - 获取当前日志配置
 * @config: 输出配置结构体指针
 *
 * Return: 0 on success, negative error code on failure
 */
int log_get_config(log_config_t *config);

/**
 * log_update_config - 动态更新日志配置
 * @config: 新的配置结构体指针
 *
 * Return: 0 on success, negative error code on failure
 */
int log_update_config(const log_config_t *config);

/**
 * log_add_filter_rule - 添加日志过滤规则
 * @rule: 过滤规则结构体指针
 *
 * Return: 0 on success, negative error code on failure
 */
int log_add_filter_rule(const log_filter_rule_t *rule);

/**
 * log_remove_filter_rule - 移除日志过滤规则
 * @index: 规则索引
 *
 * Return: 0 on success, negative error code on failure
 */
int log_remove_filter_rule(int index);

/**
 * log_clear_filter_rules - 清除所有日志过滤规则
 *
 * Return: 0 on success, negative error code on failure
 */
int log_clear_filter_rules(void);

/**
 * log_mqtt_connect - 手动连接MQTT服务器
 *
 * Return: 0 on success, negative error code on failure
 */
int log_mqtt_connect(void);

/**
 * log_mqtt_disconnect - 手动断开MQTT连接
 *
 * Return: 0 on success, negative error code on failure
 */
int log_mqtt_disconnect(void);

/**
 * log_compress_logs - 压缩日志文件
 * @input_path: 输入文件路径
 * @output_path: 输出压缩文件路径
 *
 * Return: 0 on success, negative error code on failure
 */
int log_compress_logs(const char *input_path, const char *output_path);

/**
 * log_printf - 内部日志打印函数（通常不直接调用，使用宏）
 */
void log_printf(log_level_t level, const char *tag, const char *fmt, ...);

/**
 * log_flush - 刷新所有缓冲区，确保日志输出完成
 *
 * Return: 0 on success, negative error code on failure
 */
int log_flush(void);

/**
 * log_get_stats - 获取日志统计信息
 * @total_logs: 总日志数量输出指针
 * @error_count: 错误日志数量输出指针
 * @buffer_usage: 缓冲区使用率输出指针
 *
 * Return: 0 on success, negative error code on failure
 */
int log_get_stats(uint32_t *total_logs, uint32_t *error_count, uint8_t *buffer_usage);

/**
 * log_get_drop_count - 获取异步队列/缓冲丢弃日志次数
 */
uint32_t log_get_drop_count(void);

/**
 * log_rotate_files - 手动触发日志文件轮转
 *
 * Return: 0 on success, negative error code on failure
 */
int log_rotate_files(void);

/**
 * log_upload_remote - 手动触发远程日志上传
 *
 * Return: 0 on success, negative error code on failure
 */
int log_upload_remote(void);

/**
 * log_shutdown - 安全关闭日志系统
 *
 * Return: 0 on success, negative error code on failure
 */
int log_shutdown(void);

/**
 * log_hexdump - 输出十六进制转储
 * @level: 日志等级
 * @tag: 标签
 * @data: 数据指针
 * @len: 数据长度
 */
void log_hexdump(log_level_t level, const char *tag, const uint8_t *data, size_t len);

/**
 * log_fetch_ringbuf - 从环形缓冲区读取历史日志
 * @buf: 输出缓冲区
 * @len: 缓冲区大小
 *
 * Return: 实际读取字节数
 */
size_t log_fetch_ringbuf(char *buf, size_t len);

/**
 * log_set_output - 动态设置输出目标
 */
void log_set_output(uint32_t outputs);

/**
 * log_set_level - 动态设置日志等级
 */
void log_set_level(log_level_t level);

/* 便捷宏（与之前 common/log.h 兼容）*/
#define LOG_ERROR(tag, fmt, ...) log_printf(LOG_LEVEL_ERROR, tag, fmt, ##__VA_ARGS__)
#define LOG_WARN(tag, fmt, ...)  log_printf(LOG_LEVEL_WARN,  tag, fmt, ##__VA_ARGS__)
#define LOG_INFO(tag, fmt, ...)  log_printf(LOG_LEVEL_INFO,  tag, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(tag, fmt, ...) log_printf(LOG_LEVEL_DEBUG, tag, fmt, ##__VA_ARGS__)

#endif  /* LOG_SYSTEM_H_ */
