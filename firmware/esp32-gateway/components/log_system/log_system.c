/**
 * @file log_system.c
 * @author EnterWorldDoor
 * @brief 日志系统的实现：环形缓冲 + 多输出
 */

 /* 引入全局错误码定义，提供统一的错误码管理 */
#include "global_error.h"
/* 引入目录操作相关头文件 */
#include <dirent.h>
/* 引入文件状态相关头文件 */
#include <sys/stat.h>
/* 引入错误号定义头文件 */
#include <errno.h>
/* 引入日志系统头文件，包含日志等级定义和函数声明 */
 #include "log_system.h"
/* 引入环形缓冲区头文件，提供环形缓冲区的数据结构和操作函数 */
 #include "ringbuf.h"
/* 引入UART驱动头文件，提供串口通信功能 */
 #include "driver/uart.h"
/* 引入FreeRTOS核心头文件，提供操作系统基础功能 */
 #include "freertos/FreeRTOS.h"
/* 引入FreeRTOS任务头文件，提供任务管理和同步原语 */
 #include "freertos/task.h"

/* 定义 CONFIG_FREERTOS_HZ 宏，用于 FreeRTOS 时间转换 */
#ifndef CONFIG_FREERTOS_HZ
#define CONFIG_FREERTOS_HZ 100
#endif
/* 引入标准字符串处理库，提供字符串操作函数 */
 #include <string.h>
/* 引入标准输入输出库，提供格式化输出函数 */
 #include <stdio.h>
/* 引入时间处理库，提供时间获取和格式化功能 */
 #include <time.h>
/* 引入POSIX标准库，提供文件操作和系统调用 */
 #include <unistd.h>
/* 引入文件控制头文件，提供文件描述符控制功能 */
#include <fcntl.h>

#ifdef CONFIG_LOG_MQTT_OUTPUT
/* 引入MQTT相关头文件 (仅在启用MQTT输出时) */
#include <mqtt_client.h>
#endif

/* 引入加密相关头文件 */
 #include "mbedtls/aes.h"
/* 引入Base64编码相关头文件 */
 #include "mbedtls/base64.h"
/* 引入压缩相关头文件 */
// #include "zlib.h"

/* 定义全局日志等级变量，默认为INFO级别，用于控制日志输出的详细程度 */
 static log_level_t g_log_level = LOG_LEVEL_INFO;
/* 定义全局日志输出目标变量，默认为UART输出，可通过位掩码组合多个输出目标 */
 static uint32_t g_outputs = LOG_OUTPUT_UART;
/* 定义环形缓冲区结构体实例，用于存储日志数据以供后续读取 */
 static struct ringbuf g_ringbuf;
/* 定义环形缓冲区存储指针，指向动态分配的内存池，用于存储实际日志数据 */
 static uint8_t *g_ringbuf_storage = NULL;
/* 定义互斥量句柄，用于保护日志系统资源，防止多任务并发访问导致数据竞争 */
 static SemaphoreHandle_t g_log_mutex = NULL;
/* 定义全局日志配置结构体，存储完整的日志系统配置 */
 static log_config_t g_config;
/* 定义错误回调函数指针，用于错误通知 */
 static log_error_callback_t g_error_callback = NULL;
/* 自定义输出Sink，用于测试和企业扩展 */
static log_sink_callback_t g_custom_sink = NULL;
static void *g_custom_sink_ctx = NULL;
/* 定义日志统计变量 */
 static uint32_t g_total_logs = 0;
 static uint32_t g_error_count = 0;
 static uint32_t g_drop_count = 0;
/* 定义日志系统初始化标志 */
 static bool g_initialized = false;
/* 定义文件系统日志文件句柄 */
 static FILE *g_log_file = NULL;
/* 定义当前日志文件大小 */
 static size_t g_current_file_size = 0;
/* 定义日志文件创建时间 */
 static time_t g_file_create_time = 0;

/* 定义日志队列相关变量 */
 static QueueHandle_t g_log_queue = NULL;
/* 定义日志处理任务句柄 */
 static TaskHandle_t g_log_task_handle = NULL;
/* 定义日志批量缓冲区 */
 static char *g_batch_buffer = NULL;
/* 定义批量缓冲区当前大小 */
 static size_t g_batch_buffer_size = 0;
/* 定义批量缓冲区最大大小 */
 static size_t g_batch_buffer_max = 0;

#ifdef CONFIG_LOG_MQTT_OUTPUT
/* 定义MQTT客户端句柄 */
 static esp_mqtt_client_handle_t g_mqtt_client = NULL;
/* 定义MQTT连接状态 */
 static bool g_mqtt_connected = false;
#endif

/* 日志消息结构体 */
 typedef struct {
    log_level_t level;
    char tag[32];
    char message[256];
} log_message_t;

/* 函数声明 */
static void handle_log_error(int error_code, const char *message);
static void check_file_rotation(void);
static void process_log_message(const log_message_t *message);
static uint32_t check_filter_rules(log_level_t level, const char *tag);
static void add_to_batch_buffer(const char *buffer);
static void flush_batch_buffer(void);
#ifdef CONFIG_LOG_MQTT_OUTPUT
static void output_mqtt(const char *buffer);
#endif
static void output_file(const char *buffer);
static void output_uart(const char *buffer);
static void output_ringbuf(const char *buffer);
static int compress_file(const char *input_path, const char *output_path);
static void clean_old_log_files(const char *dir_path, const char *base_name, int max_count);
#ifdef CONFIG_LOG_MQTT_OUTPUT
static int init_mqtt_client(void);
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
#endif

/* 定义获取时间戳字符串的静态函数，将当前时间格式化为指定格式 */
 static void get_timestamp_str(char *buf, size_t len)
 {
    /* 调用time函数获取当前系统时间，返回从1970年1月1日开始的秒数 */
    time_t now = time(NULL);
    /* 定义本地时间结构体变量，用于存储分解后的时间信息 */
    struct tm tm_info;
    /* 调用localtime_r函数将时间戳转换为本地时间结构，线程安全版本 */
    localtime_r(&now, &tm_info);
    /* 调用strftime函数将时间结构格式化为"年-月-日 时:分:秒"格式字符串 */
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm_info);
 }

/* 定义输出到UART串口的静态函数，使用默认的UART0端口 */
static void output_uart(const char *str)
{
    /*
     * 安全检查: 确保字符串有效且不为空
     * 避免向UART写入NULL或空指针导致崩溃
     */
    if (!str || strlen(str) == 0) {
        return;
    }
    
    /*
     * 直接调用uart_write_bytes写入UART0
     * 
     * 注意: UART0驱动已在log_system_init()中通过
     *       uart_driver_install(UART_NUM_0, ...) 安装
     * 
     * 如果驱动未安装,此函数会返回ESP_ERR_INVALID_STATE错误,
     * 但不会导致系统崩溃 (仅丢失该条日志)
     */
    uart_write_bytes(UART_NUM_0, str, strlen(str));
    uart_write_bytes(UART_NUM_0, "\r\n", 2);
}

/* 定义输出到环形缓冲区的静态函数，将日志数据存储到缓冲区供后续读取 */
 static void output_ringbuf(const char *str)
 {
    /* 检查环形缓冲区的buffer指针是否有效，确保缓冲区已初始化 */
    if (g_ringbuf.buffer) {
        /* 调用ringbuf_push函数将日志字符串压入环形缓冲区 */
        ringbuf_push(&g_ringbuf, (const uint8_t*)str, strlen(str));
    }
 }

/* 定义输出到文件系统的静态函数，将日志数据写入文件 */
 static void output_file(const char *str)
 {
    /* 检查日志文件是否有效 */
    if (!g_log_file) {
        return;
    }
    
    /* 写入日志文件 */
    int written = fprintf(g_log_file, "%s\n", str);
    if (written < 0) {
        /* 处理文件写入错误 */
        handle_log_error(APP_ERR_LOG_FILE_WRITE, "Failed to write to log file");
        return;
    }
    
    /* 更新当前文件大小 */
    g_current_file_size += written;
    
    /* 检查是否需要文件轮转 */
    check_file_rotation();
 }

/* 定义内部函数，检查是否需要文件轮转 */
 static void check_file_rotation(void)
 {
    /* 检查文件输出是否已启用 */
    if (!g_log_file) {
        return;
    }
    
    /* 检查轮转策略 */
    if (g_config.rotation_policy == LOG_ROTATION_NONE) {
        return;
    }
    
    bool need_rotate = false;
    
    /* 按大小轮转 */
    if (g_config.rotation_policy == LOG_ROTATION_SIZE || g_config.rotation_policy == LOG_ROTATION_BOTH) {
        if (g_current_file_size >= g_config.max_file_size) {
            need_rotate = true;
        }
    }
    
    /* 按时间轮转 */
    if (g_config.rotation_policy == LOG_ROTATION_TIME || g_config.rotation_policy == LOG_ROTATION_BOTH) {
        time_t now = time(NULL);
        double hours = difftime(now, g_file_create_time) / 3600.0;
        if (hours >= g_config.rotation_time_hours) {
            need_rotate = true;
        }
    }
    
    /* 执行文件轮转 */
    if (need_rotate) {
        /* 释放互斥锁以避免死锁 */
        if (g_log_mutex) xSemaphoreGive(g_log_mutex);
        /* 执行文件轮转 */
        log_rotate_files();
        /* 重新获取互斥锁 */
        if (g_log_mutex) xSemaphoreTake(g_log_mutex, portMAX_DELAY);
    }
 }

/* 定义日志处理任务函数，处理异步日志队列 */
 static void log_task(void *pvParameters)
 {
    log_message_t message;
    
    while (1) {
        /* 等待日志消息，最多等待100ms */
        if (xQueueReceive(g_log_queue, &message, pdMS_TO_TICKS(100)) == pdTRUE) {
            /* 处理收到的日志消息 */
            process_log_message(&message);
        }
    }
 }

/* 定义处理日志消息的函数 */
 static void process_log_message(const log_message_t *message)
{
    /* 检查日志等级是否高于全局配置等级 */
    if (message->level > g_log_level) {
        return;
    }
    
    /* 定义时间戳缓冲区 */
    char timestamp[32];
    get_timestamp_str(timestamp, sizeof(timestamp));
    
    /* 构建完整日志消息 */
    char buf[512];
    snprintf(buf, sizeof(buf), "[%s] [%s] %s: %s",
                       timestamp,
                       (message->level == LOG_LEVEL_ERROR) ? "ERROR" :
                       (message->level == LOG_LEVEL_WARN)  ? "WARN"  :
                       (message->level == LOG_LEVEL_INFO)  ? "INFO"  :
                       (message->level == LOG_LEVEL_DEBUG) ? "DEBUG" : "VERBOSE",
                       message->tag,
                       message->message);
    
    /* 检查过滤规则，获取允许的输出目标 */
    uint32_t allowed_outputs = check_filter_rules(message->level, message->tag);
    
    /* 处理不同的输出目标 */
    if (allowed_outputs & LOG_OUTPUT_UART) {
        output_uart(buf);
    }
    
    if (allowed_outputs & LOG_OUTPUT_RINGBUF) {
        output_ringbuf(buf);
    }
    
    if (allowed_outputs & LOG_OUTPUT_FILE) {
        if (g_config.batch_size > 0) {
            /* 批量处理模式 */
            add_to_batch_buffer(buf);
        } else {
            /* 直接输出模式 */
            output_file(buf);
        }
    }

#ifdef CONFIG_LOG_MQTT_OUTPUT
    if (allowed_outputs & LOG_OUTPUT_MQTT) {
        output_mqtt(buf);
    }
#endif
}

/* 定义添加到批量缓冲区的函数 */
 static void add_to_batch_buffer(const char *str)
 {
    size_t str_len = strlen(str) + 1; /* 包括换行符 */
    
    /* 检查批量缓冲区是否需要扩容 */
    if (g_batch_buffer_size + str_len > g_batch_buffer_max) {
        /* 扩容缓冲区 */
        size_t new_size = g_batch_buffer_size + str_len + 1024;
        char *new_buffer = realloc(g_batch_buffer, new_size);
        if (new_buffer) {
            g_batch_buffer = new_buffer;
            g_batch_buffer_max = new_size;
        } else {
            /* 内存分配失败，直接输出 */
            output_file(str);
            return;
        }
    }
    
    /* 添加到批量缓冲区 */
    strcpy(g_batch_buffer + g_batch_buffer_size, str);
    strcat(g_batch_buffer + g_batch_buffer_size, "\n");
    g_batch_buffer_size += str_len;
    
    /* 检查是否达到批量输出阈值 */
    if (g_batch_buffer_size >= g_config.batch_size) {
        flush_batch_buffer();
    }
 }

/* 定义刷新批量缓冲区的函数 */
 static void flush_batch_buffer(void)
 {
    if (g_batch_buffer_size > 0 && g_log_file) {
        /* 批量写入文件 */
        size_t written = fwrite(g_batch_buffer, 1, g_batch_buffer_size, g_log_file);
        if (written == g_batch_buffer_size) {
            /* 更新文件大小 */
            g_current_file_size += written;
            /* 检查是否需要文件轮转 */
            check_file_rotation();
        } else {
            handle_log_error(APP_ERR_LOG_FILE_WRITE, "Failed to write batch buffer");
        }
        
        /* 清空缓冲区 */
        g_batch_buffer_size = 0;
    }
 }

/* 定义初始化异步日志系统的函数 */
 static int init_async_system(void)
 {
    /* 创建日志队列 */
    g_log_queue = xQueueCreate(g_config.queue_size, sizeof(log_message_t));
    if (!g_log_queue) {
        handle_log_error(APP_ERR_LOG_QUEUE_CREATE, "Failed to create log queue");
        return APP_ERR_LOG_QUEUE_CREATE;
    }
    
    /* 分配批量缓冲区 */
    g_batch_buffer_max = g_config.batch_size * 2;
    g_batch_buffer = malloc(g_batch_buffer_max);
    if (!g_batch_buffer) {
        handle_log_error(APP_ERR_LOG_MEMORY_ALLOC, "Failed to allocate batch buffer");
        vQueueDelete(g_log_queue);
        g_log_queue = NULL;
        return APP_ERR_LOG_MEMORY_ALLOC;
    }
    g_batch_buffer_size = 0;
    
    /* 创建日志处理任务 */
    BaseType_t ret = xTaskCreate(log_task, "log_task", 4096, NULL, 5, &g_log_task_handle);
    if (ret != pdPASS) {
        handle_log_error(APP_ERR_LOG_QUEUE_CREATE, "Failed to create log task");
        free(g_batch_buffer);
        g_batch_buffer = NULL;
        vQueueDelete(g_log_queue);
        g_log_queue = NULL;
        return APP_ERR_LOG_QUEUE_CREATE;
    }
    
    return APP_ERR_OK;
 }

#ifdef CONFIG_LOG_MQTT_OUTPUT
/* 定义MQTT事件处理函数 */
 static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
 {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            g_mqtt_connected = true;
            LOG_INFO("LOG", "MQTT connected");
            break;
        case MQTT_EVENT_DISCONNECTED:
            g_mqtt_connected = false;
            LOG_INFO("LOG", "MQTT disconnected");
            break;
        case MQTT_EVENT_ERROR:
            g_mqtt_connected = false;
            if (event->error_handle) {
                if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                    LOG_ERROR("LOG", "MQTT event error: %s", esp_err_to_name(event->error_handle->esp_tls_last_esp_err));
                } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                    LOG_ERROR("LOG", "MQTT connection refused: %d", event->error_handle->connect_return_code);
                } else {
                    LOG_ERROR("LOG", "MQTT event error type: %d", event->error_handle->error_type);
                }
            } else {
                LOG_ERROR("LOG", "MQTT event error: unknown error");
            }
            break;
        default:
            break;
    }
 }

/* 定义初始化MQTT客户端的函数 */
 static int init_mqtt_client(void)
 {
    if (!g_config.mqtt_enabled) {
        return APP_ERR_OK;
    }
    
    /* 检查MQTT配置是否有效 */
    if (strlen(g_config.mqtt_broker) == 0) {
        handle_log_error(APP_ERR_LOG_CONFIG_INVALID, "MQTT broker is empty");
        return APP_ERR_LOG_CONFIG_INVALID;
    }
    
    /* 构建MQTT配置 */
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = g_config.mqtt_broker,
        .broker.address.port = g_config.mqtt_port,
        .credentials.username = g_config.mqtt_username,
        .credentials.authentication.password = g_config.mqtt_password,
        .session.keepalive = 120,
    };
    
    /* 创建MQTT客户端 */
    g_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!g_mqtt_client) {
        handle_log_error(APP_ERR_LOG_MQTT_CONNECT, "Failed to create MQTT client");
        return APP_ERR_LOG_MQTT_CONNECT;
    }
    
    /* 注册事件处理器 */
    esp_mqtt_client_register_event(g_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    
    /* 启动MQTT客户端 */
    esp_err_t ret = esp_mqtt_client_start(g_mqtt_client);
    if (ret != ESP_OK) {
        handle_log_error(APP_ERR_LOG_MQTT_CONNECT, "Failed to start MQTT client");
        esp_mqtt_client_destroy(g_mqtt_client);
        g_mqtt_client = NULL;
        return APP_ERR_LOG_MQTT_CONNECT;
    }
    
    return APP_ERR_OK;
 }

/* 定义清理MQTT客户端的函数 */
 static void cleanup_mqtt_client(void)
 {
    if (g_mqtt_client) {
        esp_mqtt_client_stop(g_mqtt_client);
        esp_mqtt_client_destroy(g_mqtt_client);
        g_mqtt_client = NULL;
        g_mqtt_connected = false;
    }
 }

#endif /* CONFIG_LOG_MQTT_OUTPUT */

/* 定义检查日志过滤规则的函数 */
 static uint32_t check_filter_rules(log_level_t level, const char *tag)
 {
    /* 默认使用全局输出配置 */
    uint32_t allowed_outputs = g_outputs;
    
    /* 检查是否启用了过滤规则 */
    if (g_config.filter_rule_count > 0) {
        for (int i = 0; i < g_config.filter_rule_count; i++) {
            const log_filter_rule_t *rule = &g_config.filter_rules[i];
            
            /* 检查规则是否启用 */
            if (!rule->enabled) {
                continue;
            }
            
            /* 检查标签匹配 */
            bool tag_match = false;
            if (strlen(rule->tag_pattern) == 0) {
                tag_match = true;
            } else {
                /* 简单的通配符匹配（仅支持*） */
                const char *pattern = rule->tag_pattern;
                const char *tag_ptr = tag;
                
                while (*pattern) {
                    if (*pattern == '*') {
                        pattern++;
                        if (!*pattern) {
                            tag_match = true;
                            break;
                        }
                        while (*tag_ptr && *tag_ptr != *pattern) {
                            tag_ptr++;
                        }
                        if (!*tag_ptr) {
                            break;
                        }
                    } else if (*pattern == *tag_ptr) {
                        pattern++;
                        tag_ptr++;
                    } else {
                        break;
                    }
                }
                
                if (!*pattern && !*tag_ptr) {
                    tag_match = true;
                }
            }
            
            /* 检查日志等级范围 */
            if (tag_match && level >= rule->min_level && level <= rule->max_level) {
                allowed_outputs = rule->enabled_outputs;
                break;
            }
        }
    }
    
    return allowed_outputs;
 }

/* 定义加密日志消息的函数 */
 

/* 定义压缩日志文件的函数 */
 static int compress_file(const char *input_path, const char *output_path)
{
    // 由于 zlib 库不可用，使用简单的文件复制作为替代
    // 实际项目中可以考虑使用 ESP-IDF 内置的压缩库或其他替代方案
    FILE *in = fopen(input_path, "rb");
    if (!in) {
        LOG_ERROR("LOG", "Failed to open input file");
        return APP_ERR_LOG_FILE_OPEN;
    }

    FILE *out = fopen(output_path, "wb");
    if (!out) {
        fclose(in);
        LOG_ERROR("LOG", "Failed to open output file");
        return APP_ERR_LOG_FILE_OPEN;
    }

    char buffer[1024];
    size_t len;

    while ((len = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (fwrite(buffer, 1, len, out) != len) {
            LOG_ERROR("LOG", "Failed to write data");
            fclose(out);
            fclose(in);
            return APP_ERR_LOG_FILE_WRITE;
        }
    }

    fclose(out);
    fclose(in);
    LOG_INFO("LOG", "File copied (compression not supported): %s -> %s", input_path, output_path);
    return APP_ERR_OK;
}

/* 定义清理异步日志系统的函数 */
 static void cleanup_async_system(void)
 {
    /* 停止日志任务 */
    if (g_log_task_handle) {
        vTaskDelete(g_log_task_handle);
        g_log_task_handle = NULL;
    }
    
    /* 删除日志队列 */
    if (g_log_queue) {
        vQueueDelete(g_log_queue);
        g_log_queue = NULL;
    }
    
    /* 释放批量缓冲区 */
    if (g_batch_buffer) {
        free(g_batch_buffer);
        g_batch_buffer = NULL;
    }
    g_batch_buffer_size = 0;
    g_batch_buffer_max = 0;
 }

/* 定义内部函数，清理旧的日志文件 */
 static void clean_old_log_files(const char *dir_path, const char *base_name, int max_count)
 {
    DIR *dir;
    struct dirent *entry;
    struct stat statbuf;
    
    /* 打开目录 */
    dir = opendir(dir_path);
    if (!dir) {
        return;
    }
    
    /* 存储日志文件信息 */
    typedef struct {
        char path[256];
        time_t mtime;
    } log_file_info_t;
    
    log_file_info_t *files = NULL;
    int file_count = 0;
    
    /* 遍历目录，收集日志文件 */
    while ((entry = readdir(dir)) != NULL) {
        /* 跳过 . 和 .. */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        /* 检查是否是日志备份文件 */
        if (strstr(entry->d_name, base_name) != NULL) {
            char full_path[256];
            snprintf(full_path, sizeof(full_path), "%s%s", dir_path, entry->d_name);
            
            /* 获取文件状态 */
            if (stat(full_path, &statbuf) == 0) {
                /* 扩展文件数组 */
                files = realloc(files, (file_count + 1) * sizeof(log_file_info_t));
                if (files) {
                    strcpy(files[file_count].path, full_path);
                    files[file_count].mtime = statbuf.st_mtime;
                    file_count++;
                }
            }
        }
    }
    
    closedir(dir);
    
    /* 如果文件数量超过限制，删除最旧的文件 */
    if (file_count > max_count) {
        /* 按修改时间排序（从旧到新） */
        for (int i = 0; i < file_count - 1; i++) {
            for (int j = i + 1; j < file_count; j++) {
                if (files[i].mtime > files[j].mtime) {
                    log_file_info_t temp = files[i];
                    files[i] = files[j];
                    files[j] = temp;
                }
            }
        }
        
        /* 删除多余的旧文件 */
        int files_to_delete = file_count - max_count;
        for (int i = 0; i < files_to_delete; i++) {
            unlink(files[i].path);
        }
    }
    
    /* 释放内存 */
    if (files) {
        free(files);
    }
 }

 /* 定义日志输出主函数，支持可变参数格式化输出，包含日志等级、标签和格式化内容 */
void log_printf(log_level_t level, const char *tag, const char *fmt, ...)
{
    /* 检查日志等级是否高于全局配置等级，如果是则直接返回，不输出该日志 */
    if (level > g_log_level) return;

    /* 增加总日志计数 */
    g_total_logs++;
    
    /* 如果是错误日志，增加错误计数 */
    if (level == LOG_LEVEL_ERROR) {
        g_error_count++;
    }

    /* 检查是否使用异步模式 */
    if (g_config.async_mode && g_log_queue) {
        /* 异步模式：将日志消息发送到队列 */
        log_message_t message;
        message.level = level;
        
        /* 复制标签，确保安全 */
        strncpy(message.tag, tag, sizeof(message.tag) - 1);
        message.tag[sizeof(message.tag) - 1] = '\0';
        
        /* 格式化消息 */
        va_list args;
        va_start(args, fmt);
        vsnprintf(message.message, sizeof(message.message), fmt, args);
        va_end(args);
        
        /* 发送到队列，非阻塞 */
        if (xQueueSend(g_log_queue, &message, 0) != pdPASS) {
            /* 队列已满，回退到同步模式 */
            handle_log_error(APP_ERR_LOG_QUEUE_SEND, "Log queue full, falling back to sync mode");
            /* 继续执行同步模式 */
        } else {
            /* 异步模式成功，直接返回 */
            return;
        }
    }

    /* 同步模式：直接处理日志 */
    /* 获取互斥锁，使用portMAX_DELAY表示无限等待，防止多任务并发写入导致数据错乱 */
    if (g_log_mutex) xSemaphoreTake(g_log_mutex, portMAX_DELAY);

    /* 定义时间戳缓冲区，用于存储格式化后的时间字符串 */
    char timestamp[32];

    /* 调用get_timestamp_str函数获取当前时间并格式化为字符串 */
    get_timestamp_str(timestamp, sizeof(timestamp));

    /* 定义日志缓冲区，用于存储完整的日志消息 */
    char buf[256];
    /* 使用snprintf函数拼接日志头部，包含时间戳、日志等级和标签 */
    int len = snprintf(buf, sizeof(buf), "[%s] [%s] %s: ",
                       timestamp,
                       /* 根据日志等级枚举值转换为对应的字符串表示 */
                       (level == LOG_LEVEL_ERROR) ? "ERROR" :
                       (level == LOG_LEVEL_WARN)  ? "WARN"  :
                       (level == LOG_LEVEL_INFO)  ? "INFO"  :
                       (level == LOG_LEVEL_DEBUG) ? "DEBUG" : "VERBOSE",
                       tag);
    /* 定义可变参数列表，用于处理格式化字符串的参数 */                    
    va_list args;
    /* 初始化可变参数列表，指向fmt参数之后的第一个可变参数 */
    va_start(args, fmt);
    /* 使用vsnprintf函数将格式化字符串和参数写入日志缓冲区 */
    vsnprintf(buf + len, sizeof(buf) - len, fmt, args);
    /* 清理可变参数列表，释放相关资源 */
    va_end(args);

    /* 检查过滤规则，获取允许的输出目标 */
    uint32_t allowed_outputs = check_filter_rules(level, tag);
    
    /* 检查是否启用UART输出，通过位与运算判断 */
    if (allowed_outputs & LOG_OUTPUT_UART) output_uart(buf);
    /* 检查是否启用环形缓冲区输出，通过位与运算判断 */
    if (allowed_outputs & LOG_OUTPUT_RINGBUF) output_ringbuf(buf);
    /* 检查是否启用文件输出，通过位与运算判断 */
    if (allowed_outputs & LOG_OUTPUT_FILE) {
        if (g_config.batch_size > 0) {
            add_to_batch_buffer(buf);
        } else {
            output_file(buf);
        }
    }
#ifdef CONFIG_LOG_MQTT_OUTPUT
    /* 检查是否启用MQTT输出，通过位与运算判断 */
    if (allowed_outputs & LOG_OUTPUT_MQTT) {
        output_mqtt(buf);
    }
#endif

    /* 释放互斥锁，允许其他任务访问日志系统资源 */
    if (g_log_mutex) xSemaphoreGive(g_log_mutex);
}

/* 定义十六进制数据转储函数，以十六进制和ASCII格式输出二进制数据 */
void log_hexdump(log_level_t level, const char *tag, const uint8_t *data, size_t len)
{
    /* 检查日志等级是否高于全局配置等级，如果是则直接返回 */
    if (level > g_log_level) return;
    /* 定义单行输出缓冲区，用于存储一行十六进制转储数据 */
    char line[80];
    /* 循环处理数据，每次处理16字节为一行 */
    for (size_t i = 0; i < len; i += 16) {
        /* 计算剩余未处理的数据字节数 */
        size_t remaining = len - i;
        /* 计算当前行实际处理的字节数，最多16字节 */
        size_t chunk = remaining > 16 ? 16 : remaining;
        /* 格式化输出偏移地址，以十六进制显示 */
        snprintf(line, sizeof(line), "%04zx: ", i);
        /* 循环输出当前行的十六进制数据 */
        for (size_t j = 0; j < chunk; j++) {
            /* 定义单个字节的十六进制字符串缓冲区 */
            char hex[4];
            /* 将单个字节格式化为两位十六进制字符串 */
            snprintf(hex, sizeof(hex), "%02x ", data[i + j]);
            /* 将十六进制字符串追加到行缓冲区 */
            strcat(line, hex);
        }
        /* 用空格填充不足16字节的部分，保持对齐 */
        for (size_t j = chunk; j < 16; j++) strcat(line, "   ");
        /* 在十六进制和ASCII表示之间添加分隔空格 */
        strcat(line, " ");
        /* 循环输出当前行的ASCII字符表示 */
        for (size_t j = 0; j < chunk; j++) {
            /* 判断字节是否为可打印ASCII字符，范围0x20-0x7E */
            char c = (data[i + j] >= 0x20 && data[i + j] < 0x7F) ? data[i + j] : '.';
            /* 创建单字符字符串 */
            char ch[2] = {c, 0};
            /* 将ASCII字符追加到行缓冲区 */
            strcat(line, ch);
        }
        /* 调用log_printf函数输出格式化后的转储行 */
        log_printf(level, tag, "%s", line);
    }
}

/* 定义日志系统初始化函数，设置日志等级、输出目标和环形缓冲区大小 */
int log_system_init(log_level_t level, uint32_t outputs, size_t ringbuf_size)
{
    /* 设置全局日志等级，控制日志输出的详细程度 */
    g_log_level = level;
    /* 设置全局日志输出目标，可通过位掩码组合多个输出方式 */
    g_outputs = outputs;
    
    /* 初始化默认配置 */
    memset(&g_config, 0, sizeof(log_config_t));
    g_config.level = level;
    g_config.outputs = outputs;
    g_config.ringbuf_size = ringbuf_size;
    g_config.rotation_policy = LOG_ROTATION_NONE;
    g_config.async_mode = false;
    g_config.queue_size = 100;
    g_config.batch_size = 4096;
    g_config.encryption_enabled = false;
    g_config.mqtt_enabled = false;
    g_config.remote_enabled = false;

    /* 检查环形缓冲区大小是否大于0 */
    if (ringbuf_size > 0) {
        /* 动态分配环形缓冲区存储空间 */
        g_ringbuf_storage = malloc(ringbuf_size);
        /* 检查内存分配是否成功 */
        if (!g_ringbuf_storage) return APP_ERR_LOG_MEMORY_ALLOC;
        /* 初始化环形缓冲区，传入缓冲区句柄、存储空间、大小和覆盖标志 */
        ringbuf_init(&g_ringbuf, g_ringbuf_storage, ringbuf_size, true);
    }

    /* 创建互斥量用于保护日志系统资源 */
    g_log_mutex = xSemaphoreCreateMutex();
    /* 检查互斥量创建是否成功 */
    if (!g_log_mutex) {
        /* 如果互斥量创建失败，释放已分配的环形缓冲区内存 */
        if (g_ringbuf_storage) free(g_ringbuf_storage);
        /* 返回错误代码 */
        return APP_ERR_LOG_MUTEX_CREATE;
    }

    /*
     * ========== 初始化 UART0 驱动 (用于日志输出) ==========
     *
     * ⚠️ 【关键】必须在使用 uart_write_bytes() 之前安装驱动!
     *   原因: ESP-IDF 的 UART 驱动需要显式安装
     *   否则调用 uart_write_bytes() 会返回 ESP_ERR_INVALID_STATE (1629)
     *
     * 配置参数:
     *   - UART_NUM_0: 使用UART0 (ESP32的默认日志串口)
     *   - RX缓冲区: 256 bytes (仅用于日志输出,不需要接收)
     *   - TX缓冲区: 2048 bytes (足够大的输出缓冲)
     *   - 事件队列大小: 0 (不需要事件回调)
     *   - 事件标志: 0
     *   - 分配标志: 0
     */
    if (outputs & LOG_OUTPUT_UART) {
        esp_err_t ret = uart_driver_install(
            UART_NUM_0,      // UART端口号 (0=默认日志端口)
            256,             // RX缓冲区大小 (bytes)
            2048,            // TX缓冲区大小 (bytes)
            0,               // 事件队列大小
            0,               // 事件队列标志
            0                // 中断分配标志
        );
        
        if (ret != ESP_OK) {
            /* UART驱动安装失败不是致命的,回退到printf */
            printf("[LOG_WARN] Failed to install UART0 driver (err=0x%x), using printf fallback\n", ret);
            /* 禁用UART输出,避免后续错误 */
            outputs &= ~LOG_OUTPUT_UART;
            g_outputs &= ~LOG_OUTPUT_UART;
        }
    }

    /* 标记日志系统已初始化 */
    g_initialized = true;

    /* 如果启用了异步模式，初始化异步系统 */
    if (g_config.async_mode) {
        int ret = init_async_system();
        if (ret != APP_ERR_OK) {
            /* 异步初始化失败，回退到同步模式 */
            g_config.async_mode = false;
            handle_log_error(ret, "Failed to initialize async system, falling back to sync mode");
        }
    }
    
#ifdef CONFIG_LOG_MQTT_OUTPUT
    /* 如果启用了MQTT，初始化MQTT客户端 */
    if (g_config.mqtt_enabled) {
        int ret = init_mqtt_client();
        if (ret != APP_ERR_OK) {
            /* MQTT初始化失败，禁用MQTT */
            g_config.mqtt_enabled = false;
            handle_log_error(ret, "Failed to initialize MQTT client, disabling MQTT");
        }
    }
#endif

    /* 使用LOG_INFO宏输出日志系统初始化成功的消息 */
    LOG_INFO("LOG", "Log system initialized (level=%d, outputs=0x%x, async=%d, batch=%d, mqtt=%d, filter=%d)", 
             level, outputs, g_config.async_mode, g_config.batch_size, g_config.mqtt_enabled, g_config.filter_rule_count);
    /* 返回成功代码 */
    return APP_ERR_OK;
}

/* 定义从环形缓冲区获取日志数据的函数，用于读取存储的日志 */
size_t log_fetch_ringbuf(char *buf, size_t len)
{
    /* 检查环形缓冲区是否已初始化 */
    if (!g_ringbuf.buffer) return 0;
    /* 调用ringbuf_pop函数从环形缓冲区弹出数据到指定缓冲区 */
    return ringbuf_pop(&g_ringbuf, (uint8_t*)buf, len);
}

/* 定义设置错误回调函数，用于错误通知机制 */
void log_set_error_callback(log_error_callback_t callback)
{
    /* 设置错误回调函数指针 */
    g_error_callback = callback;
}

/* 定义内部错误处理函数，统一处理日志系统错误 */
static void handle_log_error(int error_code, const char *error_msg)
{
    /* 增加错误计数 */
    g_error_count++;
    /* 如果设置了错误回调，则调用回调函数通知错误 */
    if (g_error_callback) {
        g_error_callback(error_code, error_msg);
    }
}

/* 定义使用配置结构体初始化日志系统的高级函数 */
int log_system_init_with_config(const log_config_t *config)
{
    /* 检查配置指针是否有效 */
    if (!config) {
        handle_log_error(APP_ERR_LOG_INVALID_PARAM, "Invalid config parameter");
        return APP_ERR_LOG_INVALID_PARAM;
    }
    
    /* 检查日志等级是否有效 */
    if (config->level < LOG_LEVEL_ERROR || config->level > LOG_LEVEL_VERBOSE) {
        handle_log_error(APP_ERR_LOG_INVALID_PARAM, "Invalid log level");
        return APP_ERR_LOG_INVALID_PARAM;
    }
    
    /* 检查输出目标是否有效 */
    if (config->outputs == 0) {
        handle_log_error(APP_ERR_LOG_INVALID_PARAM, "No output targets specified");
        return APP_ERR_LOG_INVALID_PARAM;
    }
    
    /* 复制配置到全局配置结构体 */
    memcpy(&g_config, config, sizeof(log_config_t));
    
    /* 调用基础初始化函数 */
    int ret = log_system_init(config->level, config->outputs, config->ringbuf_size);
    if (ret != APP_ERR_OK) {
        handle_log_error(ret, "Failed to initialize log system");
        return ret;
    }
    
    /* 如果启用了文件输出，初始化文件系统 */
    if (config->outputs & LOG_OUTPUT_FILE) {
        /* 检查文件路径是否有效 */
        if (strlen(config->log_file_path) == 0) {
            handle_log_error(APP_ERR_LOG_INVALID_PARAM, "Log file path is empty");
        return APP_ERR_LOG_INVALID_PARAM;
        }
        
        /* 打开日志文件，追加模式 */
        g_log_file = fopen(config->log_file_path, "a");
        if (!g_log_file) {
            handle_log_error(APP_ERR_LOG_FILE_OPEN, "Failed to open log file");
        return APP_ERR_LOG_FILE_OPEN;
        }
        
        /* 获取当前文件大小 */
        fseek(g_log_file, 0, SEEK_END);
        g_current_file_size = ftell(g_log_file);
        fseek(g_log_file, 0, SEEK_SET);
        
        /* 记录文件创建时间 */
        g_file_create_time = time(NULL);
    }
    
    /* 标记日志系统已初始化 */
    g_initialized = true;
    
    return APP_ERR_OK;
}

/* 定义获取当前日志配置的函数 */
int log_get_config(log_config_t *config)
{
    /* 检查配置指针是否有效 */
    if (!config) {
        handle_log_error(APP_ERR_LOG_INVALID_PARAM, "Invalid config parameter");
        return APP_ERR_LOG_INVALID_PARAM;
    }
    
    /* 检查日志系统是否已初始化 */
    if (!g_initialized) {
        handle_log_error(APP_ERR_LOG_INIT_FAIL, "Log system not initialized");
        return APP_ERR_LOG_INIT_FAIL;
    }
    
    /* 复制当前配置到输出结构体 */
    memcpy(config, &g_config, sizeof(log_config_t));
    
    return APP_ERR_OK;
}

/* 定义动态更新日志配置的函数 */
int log_update_config(const log_config_t *config)
{
    /* 检查配置指针是否有效 */
    if (!config) {
        handle_log_error(APP_ERR_LOG_INVALID_PARAM, "Invalid config parameter");
        return APP_ERR_LOG_INVALID_PARAM;
    }
    
    /* 检查日志系统是否已初始化 */
    if (!g_initialized) {
        handle_log_error(APP_ERR_LOG_INIT_FAIL, "Log system not initialized");
        return APP_ERR_LOG_INIT_FAIL;
    }
    
    /* 获取互斥锁保护配置更新 */
    if (g_log_mutex) xSemaphoreTake(g_log_mutex, portMAX_DELAY);
    
    /* 更新日志等级 */
    if (config->level >= LOG_LEVEL_ERROR && config->level <= LOG_LEVEL_VERBOSE) {
        g_log_level = config->level;
        g_config.level = config->level;
    }
    
    /* 更新输出目标 */
    if (config->outputs != 0) {
        g_outputs = config->outputs;
        g_config.outputs = config->outputs;
    }
    
    /* 释放互斥锁 */
    if (g_log_mutex) xSemaphoreGive(g_log_mutex);
    
    return APP_ERR_OK;
}

/* 定义刷新所有缓冲区的函数，确保日志输出完成 */
int log_flush(void)
{
    /* 检查日志系统是否已初始化 */
    if (!g_initialized) {
        handle_log_error(APP_ERR_LOG_INIT_FAIL, "Log system not initialized");
        return APP_ERR_LOG_INIT_FAIL;
    }
    
    /* 获取互斥锁保护刷新操作 */
    if (g_log_mutex) xSemaphoreTake(g_log_mutex, portMAX_DELAY);
    
    /* 如果启用了批量处理，刷新批量缓冲区 */
    if (g_config.batch_size > 0) {
        flush_batch_buffer();
    }
    
    /* 如果文件输出已启用，刷新文件缓冲区 */
    if (g_log_file) {
        fflush(g_log_file);
        fsync(fileno(g_log_file));
    }
    
    /* 释放互斥锁 */
    if (g_log_mutex) xSemaphoreGive(g_log_mutex);
    
    return APP_ERR_OK;
}

/* 定义获取日志统计信息的函数 */
int log_get_stats(uint32_t *total_logs, uint32_t *error_count, uint8_t *buffer_usage)
{
    /* 检查日志系统是否已初始化 */
    if (!g_initialized) {
        handle_log_error(APP_ERR_LOG_INIT_FAIL, "Log system not initialized");
        return APP_ERR_LOG_INIT_FAIL;
    }
    
    /* 获取互斥锁保护统计信息读取 */
    if (g_log_mutex) xSemaphoreTake(g_log_mutex, portMAX_DELAY);
    
    /* 输出总日志数量 */
    if (total_logs) {
        *total_logs = g_total_logs;
    }
    
    /* 输出错误日志数量 */
    if (error_count) {
        *error_count = g_error_count;
    }
    
    /* 计算并输出缓冲区使用率 */
    if (buffer_usage) {
        if (g_ringbuf.buffer && g_config.ringbuf_size > 0) {
            *buffer_usage = (uint8_t)((g_ringbuf.size * 100) / g_config.ringbuf_size);
        } else {
            *buffer_usage = 0;
        }
    }
    
    /* 释放互斥锁 */
    if (g_log_mutex) xSemaphoreGive(g_log_mutex);
    
    return APP_ERR_OK;
}

/* 定义手动触发日志文件轮转的函数 */
int log_rotate_files(void)
{
    /* 检查日志系统是否已初始化 */
    if (!g_initialized) {
        handle_log_error(APP_ERR_LOG_INIT_FAIL, "Log system not initialized");
        return APP_ERR_LOG_INIT_FAIL;
    }
    
    /* 检查文件输出是否已启用 */
    if (!g_log_file) {
        handle_log_error(APP_ERR_LOG_FILE_ROTATE, "File logging not enabled");
        return APP_ERR_LOG_FILE_ROTATE;
    }
    
    /* 获取互斥锁保护文件轮转操作 */
    if (g_log_mutex) xSemaphoreTake(g_log_mutex, portMAX_DELAY);
    
    /* 关闭当前日志文件 */
    if (g_log_file) {
        fflush(g_log_file);
        fsync(fileno(g_log_file));
        fclose(g_log_file);
        g_log_file = NULL;
    }
    
    /* 生成备份文件名 */
    char backup_path[512];
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    
    /* 提取文件名和路径 */
    char *last_slash = strrchr(g_config.log_file_path, '/');
    char base_name[128];
    char dir_path[256];
    
    if (last_slash) {
        strncpy(dir_path, g_config.log_file_path, last_slash - g_config.log_file_path + 1);
        dir_path[last_slash - g_config.log_file_path + 1] = '\0';
        strcpy(base_name, last_slash + 1);
    } else {
        strcpy(dir_path, "./");
        strcpy(base_name, g_config.log_file_path);
    }
    
    /* 生成带时间戳的备份文件名 */
    snprintf(backup_path, sizeof(backup_path), "%s%04d%02d%02d_%02d%02d%02d_%s",
             dir_path,
             tm_info.tm_year + 1900,
             tm_info.tm_mon + 1,
             tm_info.tm_mday,
             tm_info.tm_hour,
             tm_info.tm_min,
             tm_info.tm_sec,
             base_name);
    
    /* 重命名当前日志文件为备份文件 */
    if (rename(g_config.log_file_path, backup_path) != 0) {
        int err = errno;
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "Failed to rotate log file: %s", strerror(err));
        handle_log_error(APP_ERR_LOG_FILE_ROTATE, error_msg);
        
        /* 尝试重新打开原文件 */
        g_log_file = fopen(g_config.log_file_path, "a");
        if (!g_log_file) {
            handle_log_error(APP_ERR_LOG_FILE_OPEN, "Failed to reopen log file");
        } else {
            fseek(g_log_file, 0, SEEK_END);
            g_current_file_size = ftell(g_log_file);
        }
        
        if (g_log_mutex) xSemaphoreGive(g_log_mutex);
        return APP_ERR_LOG_FILE_ROTATE;
    }
    
    /* 打开新的日志文件 */
    g_log_file = fopen(g_config.log_file_path, "a");
    if (!g_log_file) {
        int err = errno;
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "Failed to create new log file: %s", strerror(err));
        handle_log_error(APP_ERR_LOG_FILE_OPEN, error_msg);
        
        /* 尝试恢复备份文件 */
        rename(backup_path, g_config.log_file_path);
        g_log_file = fopen(g_config.log_file_path, "a");
        if (g_log_file) {
            fseek(g_log_file, 0, SEEK_END);
            g_current_file_size = ftell(g_log_file);
        }
        
        if (g_log_mutex) xSemaphoreGive(g_log_mutex);
        return APP_ERR_LOG_FILE_OPEN;
    }
    
    /* 重置文件大小和时间 */
    g_current_file_size = 0;
    g_file_create_time = time(NULL);
    
    /* 清理旧的日志文件，保持文件数量不超过限制 */
    if (g_config.max_file_count > 0) {
        clean_old_log_files(dir_path, base_name, g_config.max_file_count);
    }
    
    /* 释放互斥锁 */
    if (g_log_mutex) xSemaphoreGive(g_log_mutex);
    
    return APP_ERR_OK;
}

/* 定义手动触发远程日志上传的函数 */
int log_upload_remote(void)
{
    /* 检查日志系统是否已初始化 */
    if (!g_initialized) {
        handle_log_error(APP_ERR_LOG_INIT_FAIL, "Log system not initialized");
        return APP_ERR_LOG_INIT_FAIL;
    }
    
    /* 检查远程输出是否已启用 */
    if (!g_config.remote_enabled) {
        handle_log_error(APP_ERR_LOG_CONFIG_INVALID, "Remote logging not enabled");
        return APP_ERR_LOG_CONFIG_INVALID;
    }
    
    /* 检查远程URL是否有效 */
    if (strlen(g_config.remote_url) == 0) {
        handle_log_error(APP_ERR_LOG_CONFIG_INVALID, "Remote URL is empty");
        return APP_ERR_LOG_CONFIG_INVALID;
    }
    
    /* TODO: 实现远程日志上传功能 */
    /* 这里需要实现HTTP/HTTPS上传逻辑 */
    /* 包括：从环形缓冲区读取日志、压缩、上传等 */
    
    return APP_ERR_OK;
}

/* 定义安全关闭日志系统的函数 */
int log_shutdown(void)
{
    /* 检查日志系统是否已初始化 */
    if (!g_initialized) {
        handle_log_error(APP_ERR_LOG_INIT_FAIL, "Log system not initialized");
        return APP_ERR_LOG_INIT_FAIL;
    }
    
    /* 获取互斥锁保护关闭操作 */
    if (g_log_mutex) xSemaphoreTake(g_log_mutex, portMAX_DELAY);
    
    /* 刷新所有缓冲区 */
    if (g_config.batch_size > 0) {
        flush_batch_buffer();
    }
    
    if (g_log_file) {
        fflush(g_log_file);
        fsync(fileno(g_log_file));
    }
    
    /* 关闭日志文件 */
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
    
#ifdef CONFIG_LOG_MQTT_OUTPUT
    /* 清理MQTT客户端 */
    if (g_config.mqtt_enabled) {
        cleanup_mqtt_client();
    }
#endif
    
    /* 清理异步系统 */
    if (g_config.async_mode) {
        cleanup_async_system();
    }
    
    /* 释放环形缓冲区内存 */
    if (g_ringbuf_storage) {
        free(g_ringbuf_storage);
        g_ringbuf_storage = NULL;
    }
    
    /* 删除互斥量 */
    if (g_log_mutex) {
        vSemaphoreDelete(g_log_mutex);
        g_log_mutex = NULL;
    }
    
    /* 标记日志系统未初始化 */
    g_initialized = false;
    
    /* 释放互斥锁 */
    if (g_log_mutex) xSemaphoreGive(g_log_mutex);
    
    return APP_ERR_OK;
}

/* 定义设置日志输出目标的函数，运行时动态修改日志输出方式 */
void log_set_output(uint32_t outputs) { g_outputs = outputs; }
/* 定义设置日志等级的函数，运行时动态调整日志输出的详细程度 */
void log_set_level(log_level_t level) { g_log_level = level; }

/* 定义添加日志过滤规则的函数 */
int log_add_filter_rule(const log_filter_rule_t *rule)
{
    /* 检查日志系统是否已初始化 */
    if (!g_initialized) {
        handle_log_error(APP_ERR_LOG_INIT_FAIL, "Log system not initialized");
        return APP_ERR_LOG_INIT_FAIL;
    }
    
    /* 检查规则指针是否有效 */
    if (!rule) {
        handle_log_error(APP_ERR_LOG_INVALID_PARAM, "Invalid filter rule");
        return APP_ERR_LOG_INVALID_PARAM;
    }
    
    /* 检查规则数量是否达到上限 */
    if (g_config.filter_rule_count >= 10) {
        handle_log_error(APP_ERR_LOG_INVALID_PARAM, "Filter rule count exceeds limit");
        return APP_ERR_LOG_INVALID_PARAM;
    }
    
    /* 获取互斥锁保护规则添加 */
    if (g_log_mutex) xSemaphoreTake(g_log_mutex, portMAX_DELAY);
    
    /* 添加规则 */
    memcpy(&g_config.filter_rules[g_config.filter_rule_count], rule, sizeof(log_filter_rule_t));
    g_config.filter_rule_count++;
    
    /* 释放互斥锁 */
    if (g_log_mutex) xSemaphoreGive(g_log_mutex);
    
    return APP_ERR_OK;
}

/* 定义移除日志过滤规则的函数 */
int log_remove_filter_rule(int index)
{
    /* 检查日志系统是否已初始化 */
    if (!g_initialized) {
        handle_log_error(APP_ERR_LOG_INIT_FAIL, "Log system not initialized");
        return APP_ERR_LOG_INIT_FAIL;
    }
    
    /* 检查索引是否有效 */
    if (index < 0 || index >= g_config.filter_rule_count) {
        handle_log_error(APP_ERR_LOG_INVALID_PARAM, "Invalid filter rule index");
        return APP_ERR_LOG_INVALID_PARAM;
    }
    
    /* 获取互斥锁保护规则移除 */
    if (g_log_mutex) xSemaphoreTake(g_log_mutex, portMAX_DELAY);
    
    /* 移除规则 */
    for (int i = index; i < g_config.filter_rule_count - 1; i++) {
        memcpy(&g_config.filter_rules[i], &g_config.filter_rules[i + 1], sizeof(log_filter_rule_t));
    }
    g_config.filter_rule_count--;
    
    /* 释放互斥锁 */
    if (g_log_mutex) xSemaphoreGive(g_log_mutex);
    
    return APP_ERR_OK;
}

/* 定义清除所有日志过滤规则的函数 */
int log_clear_filter_rules(void)
{
    /* 检查日志系统是否已初始化 */
    if (!g_initialized) {
        handle_log_error(APP_ERR_LOG_INIT_FAIL, "Log system not initialized");
        return APP_ERR_LOG_INIT_FAIL;
    }
    
    /* 获取互斥锁保护规则清除 */
    if (g_log_mutex) xSemaphoreTake(g_log_mutex, portMAX_DELAY);
    
    /* 清除所有规则 */
    g_config.filter_rule_count = 0;
    
    /* 释放互斥锁 */
    if (g_log_mutex) xSemaphoreGive(g_log_mutex);
    
    return APP_ERR_OK;
}

/* 定义手动连接MQTT服务器的函数 */
#ifdef CONFIG_LOG_MQTT_OUTPUT
int log_mqtt_connect(void)
{
    /* 检查日志系统是否已初始化 */
    if (!g_initialized) {
        handle_log_error(APP_ERR_LOG_INIT_FAIL, "Log system not initialized");
        return APP_ERR_LOG_INIT_FAIL;
    }
    
    /* 检查MQTT是否已启用 */
    if (!g_config.mqtt_enabled) {
        handle_log_error(APP_ERR_LOG_CONFIG_INVALID, "MQTT not enabled");
        return APP_ERR_LOG_CONFIG_INVALID;
    }
    
    /* 如果已经连接，直接返回 */
    if (g_mqtt_connected) {
        return APP_ERR_OK;
    }
    
    /* 重新初始化MQTT客户端 */
    cleanup_mqtt_client();
    return init_mqtt_client();
}

/* 定义手动断开MQTT连接的函数 */
int log_mqtt_disconnect(void)
{
    /* 检查日志系统是否已初始化 */
    if (!g_initialized) {
        handle_log_error(APP_ERR_LOG_INIT_FAIL, "Log system not initialized");
        return APP_ERR_LOG_INIT_FAIL;
    }
    
    /* 清理MQTT客户端 */
    cleanup_mqtt_client();
    return APP_ERR_OK;
}
#else
int log_mqtt_connect(void)
{
    (void)0;
    return APP_ERR_LOG_CONFIG_INVALID;
}

int log_mqtt_disconnect(void)
{
    (void)0;
    return APP_ERR_OK;
}
#endif

/* 定义压缩日志文件的函数 */
int log_compress_logs(const char *input_path, const char *output_path)
{
    /* 检查日志系统是否已初始化 */
    if (!g_initialized) {
        handle_log_error(APP_ERR_LOG_INIT_FAIL, "Log system not initialized");
        return APP_ERR_LOG_INIT_FAIL;
    }
    
    /* 检查输入和输出路径 */
    if (!input_path || !output_path) {
        handle_log_error(APP_ERR_LOG_INVALID_PARAM, "Invalid file paths");
        return APP_ERR_LOG_INVALID_PARAM;
    }
    
    /* 检查压缩是否已启用 */
    if (!g_config.compression_enabled) {
        handle_log_error(APP_ERR_LOG_CONFIG_INVALID, "Compression not enabled");
        return APP_ERR_LOG_CONFIG_INVALID;
    }
    
    /* 执行压缩 */
    return compress_file(input_path, output_path);
}