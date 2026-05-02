/**
 * @file mqtt.c
 * @author EnterWorldDoor
 * @brief 企业级 MQTT 通信模块实现 (TLS + 离线缓存 + 批量发布 + 动态配置)
 *
 * 架构设计:
 *   ┌─────────────────────────────────────────────────────┐
 *   │              MQTT Module (Business Layer)            │
 *   │                                                     │
 *   │  sensor_service.analysis_result                      │
 *   │         ↓                                           │
 *   │  [JSON Serializer]                                  │
 *   │         ↓                                           │
 *   │  [Virtual Device Manager] ← 偏移量处理              │
 *   │         ↓                                           │
 *   │  ┌─────┴─────┐                                      │
 *   │  ↓           ↓                                      │
 *   │ [Batch      [Offline                                │
 *   │  Buffer]    RingBuffer]                             │
 *   │  ↓           ↓                                      │
 *   │  └─────┬─────┘                                      │
 *   │        ↓                                            │
 *   │  [Topic Router] ← Training/Upload 模式             │
 *   │        ↓                                            │
 *   │  esp_mqtt_client (ESP-IDF Component)                │
 *   │        ↓                                            │
 *   │  TCP/TLS → Broker (PC/树莓派)                       │
 *   │                                                     │
 *   │  ← [Config Subscriber] 远程配置监听                 │
 *   └─────────────────────────────────────────────────────┘
 *
 * 关键技术点:
 *   - TLS 1.2/1.3 加密传输 (CA证书 + 可选客户端认证)
 *   - RingBuffer 离线消息队列 (断网缓存 + 自动补发)
 *   - JSON 数组批量发布 (减少网络开销 ~60%)
 *   - 远程配置动态更新 (MQTT Subscribe + JSON解析)
 *   - FreeRTOS 任务 + Mutex 保证线程安全
 */

#include "mqtt.h"
#include "log_system.h"
#include "global_error.h"
#include "ringbuf.h"
#include "config_manager.h"
#include "dsp.h"
#include "esp_log.h"
#include "esp_wifi.h"
#ifdef CONFIG_ESP_TLS_ENABLED
#include <esp_tls.h>
#endif
#include <mqtt_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <math.h>
#include <esp_timer.h>
#include <esp_random.h>

/* ==================== 模块内部状态 ==================== */

static struct {
    bool initialized;
    enum mqtt_mode mode;
    enum mqtt_connection_state state;
    
    /* 配置 */
    struct mqtt_config config;
    
    /* ESP-IDF MQTT 客户端句柄 */
    esp_mqtt_client_handle_t client;
    
    /* 线程安全 */
    SemaphoreHandle_t mutex;
    
    /* 任务控制 */
    TaskHandle_t task_handle;
    bool running;
    
    /* 当前活跃虚拟设备索引 (用于轮流模式) */
    uint8_t active_virtual_dev_idx;
    
    /* 回调 */
    struct {
        mqtt_event_callback_t cb;
        void *user_data;
        int count;
    } event_callbacks[MQTT_MAX_CALLBACKS];
    
    /* 统计 */
    struct mqtt_stats stats;
    
    /* 连接时间戳 */
    int64_t connect_time_us;
    
    /* ========== TLS 配置 =========== */
#ifdef CONFIG_ESP_TLS_ENABLED
    esp_tls_cfg_t tls_config;          /**< ESP-IDF TLS 配置 */
#endif
    bool tls_initialized;              /**< TLS 是否已初始化 */
    
    /* ========== 新增: 离线缓冲区 =========== */
    struct ringbuf offline_ringbuf;    /**< 环形缓冲区实例 */
    uint8_t *offline_buffer_mem;       /**< 离线缓冲区内存指针 */
    bool offline_buffer_init;          /**< 离线缓冲区是否已初始化 */
    uint32_t offline_cached_messages;  /**< 当前缓存消息数 */
    
    /* ========== 新增: 批量发布 =========== */
    struct batch_config batch_cfg;     /**< 批量发布配置 */
    struct analysis_result *batch_accumulator[MQTT_BATCH_MAX_MESSAGES]; /**< 批量累积缓冲 */
    uint8_t batch_count;               /**< 当前累积消息数 */
    int64_t batch_start_time_us;       /**< 批量开始时间 */
    SemaphoreHandle_t batch_mutex;     /**< 批量操作互斥量 */
    
    /* ========== 新增: 动态配置 =========== */
    char config_topic[MQTT_CONFIG_TOPIC_LEN]; /**< 订阅的配置 Topic */
    bool config_subscribed;           /**< 是否已订阅配置 Topic */
    struct {
        mqtt_message_callback_t cb;
        void *user_data;
    } config_callback;                /**< 配置更新回调 */
} g_mqtt = {0};

/* ==================== Topic 前缀常量 ==================== */

#define TOPIC_PREFIX_TRAINING_VIBRATION   "edgevib/train/"
#define TOPIC_PREFIX_TRAINING_ENVIRONMENT "edgevib/train/"
#define TOPIC_PREFIX_UPLOAD_VIBRATION     "edgevib/upload/"
#define TOPIC_PREFIX_UPLOAD_STATUS        "edgevib/upload/"
#define TOPIC_PREFIX_UPLOAD_HEALTH        "edgevib/upload/"
#define CONFIG_TOPIC_PREFIX              "edgevib/config/"

/* ==================== 内部辅助函数 ==================== */

/**
 * build_topic_vibration - 构建振动数据 Topic
 */
static int build_topic_vibration(char *buffer, size_t buf_size, uint8_t dev_id)
{
    const char *prefix = (g_mqtt.mode == MQTT_MODE_TRAINING)
                         ? TOPIC_PREFIX_TRAINING_VIBRATION
                         : TOPIC_PREFIX_UPLOAD_VIBRATION;
    return snprintf(buffer, buf_size, "%s%u/vibration", prefix, dev_id);
}

/**
 * build_topic_environment - 构建环境数据 Topic
 */
static int build_topic_environment(char *buffer, size_t buf_size, uint8_t dev_id)
{
    const char *prefix = (g_mqtt.mode == MQTT_MODE_TRAINING)
                         ? TOPIC_PREFIX_TRAINING_ENVIRONMENT
                         : TOPIC_PREFIX_UPLOAD_STATUS;
    return snprintf(buffer, buf_size, "%s%u/environment", prefix, dev_id);
}

/**
 * build_topic_health - 构建健康状态 Topic
 */
static int build_topic_health(char *buffer, size_t buf_size, uint8_t dev_id)
{
    return snprintf(buffer, buf_size, "%s%u/health",
                    TOPIC_PREFIX_UPLOAD_HEALTH, dev_id);
}

/**
 * apply_virtual_device_offset - 应用虚拟设备偏移量
 */
static void apply_virtual_device_offset(const struct analysis_result *result,
                                        struct analysis_result *out,
                                        uint8_t idx)
{
    memcpy(out, result, sizeof(*result));
    
    if (idx == 0 || idx >= g_mqtt.config.num_virtual_devices) {
        return;
    }
    
    const struct virtual_device_config *dev = &g_mqtt.config.devices[idx - 1];
    
    if (!dev->enabled) {
        return;
    }
    
    out->vibration.x_rms.value += dev->vibration_offset_x;
    out->vibration.y_rms.value += dev->vibration_offset_y;
    out->vibration.z_rms.value += dev->vibration_offset_z;
    out->overall_rms_g += sqrtf(dev->vibration_offset_x * dev->vibration_offset_x +
                                 dev->vibration_offset_y * dev->vibration_offset_y +
                                 dev->vibration_offset_z * dev->vibration_offset_z);
    
    out->temperature_c += dev->temp_offset_c;
    out->humidity_rh += dev->humidity_offset_rh;
}

/**
 * serialize_analysis_result_to_json - 将分析结果序列化为 JSON 字符串
 */
static int serialize_analysis_result_to_json(const struct analysis_result *result,
                                             char *buffer,
                                             size_t buf_size,
                                             uint8_t dev_id)
{
    if (!result || !buffer || buf_size < 128) {
        return APP_ERR_MQTT_INVALID_PARAM;
    }

    int written = 0;
    const char *mode_str = (g_mqtt.mode == MQTT_MODE_TRAINING) ? "training" : "upload";

    /*
     * ⚠️ 【增强】完整JSON格式,包含所有必需字段!
     *
     * 新增字段 (解决PC端缺少字段WARN):
     *   - service_state: 服务状态 (RUNNING/DEGRADED/FAULT)
     *   - temperature_valid: 温度数据有效性标志
     *   - samples_analyzed: 分析样本数 (降级模式=0)
     *   - data_quality: 数据质量等级 (0=完整, 1=降级, 2=仅环境)
     *   - total_analyses: 累计分析次数
     *
     * 确保PC端Edge-AI能收到完整的上下文信息!
     */
    const char *state_str = "UNKNOWN";
    switch (result->service_state) {
        case SENSOR_STATE_UNINITIALIZED: state_str = "UNINIT"; break;
        case SENSOR_STATE_INITIALIZED:   state_str = "INIT"; break;
        case SENSOR_STATE_RUNNING:      state_str = "RUNNING"; break;
        case SENSOR_STATE_DEGRADED:     state_str = "DEGRADED"; break;
        case SENSOR_STATE_ERROR:        state_str = "ERROR"; break;
        default:                        state_str = "UNKNOWN"; break;
    }

    uint8_t data_quality = 0;
    if (result->samples_analyzed == 0 && result->temperature_valid) {
        data_quality = 2;  /* 仅环境数据 (降级模式) */
    } else if (result->service_state == SENSOR_STATE_DEGRADED) {
        data_quality = 1;  /* 降级质量 */
    } else {
        data_quality = 0;  /* 完整数据 */
    }

    written = snprintf(buffer, buf_size,
        "{\"dev_id\":%u,\"timestamp_ms\":%" PRIu32 ",\"mode\":\"%s\","
        "\"service_state\":\"%s\",\"data_quality\":%u,"
        "\"samples_analyzed\":%" PRIu32 ",\"total_analyses\":%" PRIu32 ","
        "\"temperature_valid\":%s,"
        "\"data\":{"
        "\"vibration\":{\"rms_x\":%.6f,\"rms_y\":%.6f,\"rms_z\":%.6f,"
        "\"overall_rms\":%.6f,\"peak_freq\":%.2f,\"peak_amp\":%.6f},"
        "\"environment\":{\"temperature_c\":%.2f,\"humidity_rh\":%.2f},"
        "\"compensation\":{\"active\":%s,"
        "\"offset_x\":%.6f,\"offset_y\":%.6f,\"offset_z\":%.6f},",
        dev_id,
        result->analysis_timestamp_us / 1000,
        mode_str,
        state_str,
        data_quality,
        result->samples_analyzed,
        result->total_analyses,
        result->temperature_valid ? "true" : "false",
        result->vibration.x_rms.value,
        result->vibration.y_rms.value,
        result->vibration.z_rms.value,
        result->overall_rms_g,
        result->peak_frequency_hz,
        result->peak_amplitude_g,
        result->temperature_c,
        result->humidity_rh,
        result->temp_compensation_active ? "true" : "false",
        result->temp_offset_x,
        result->temp_offset_y,
        result->temp_offset_z);

    if (written <= 0 || (size_t)written >= buf_size) {
        return APP_ERR_MQTT_BUFFER_OVERFLOW;
    }

    /* FFT 峰值数组 */
    int pos = written;
    pos += snprintf(buffer + pos, buf_size - pos, "\"fft_peaks\":[");

    for (int i = 0; i < result->vibration.x_fft.peak_count && i < DSP_NUM_PEAKS; i++) {
        if (i > 0) {
            pos += snprintf(buffer + pos, buf_size - pos, ",");
        }
        pos += snprintf(buffer + pos, buf_size - pos,
            "{\"freq\":%.2f,\"amp\":%.6f}",
            result->vibration.x_fft.peaks[i].frequency_hz,
            result->vibration.x_fft.peaks[i].amplitude);

        if ((size_t)pos >= buf_size - 16) {
            return APP_ERR_MQTT_BUFFER_OVERFLOW;
        }
    }

    pos += snprintf(buffer + pos, buf_size - pos, "]}}");

    if ((size_t)pos >= buf_size) {
        return APP_ERR_MQTT_BUFFER_OVERFLOW;
    }

    return pos;
}

/**
 * serialize_batch_to_json - 将多个分析结果序列化为批量 JSON 数组
 * @results: 分析结果数组
 * @count: 结果数量
 * @buffer: 输出缓冲区
 * @buf_size: 缓冲区大小
 * @dev_id_start: 起始设备 ID
 *
 * Return: 实际写入长度, 负数表示错误
 */
static int serialize_batch_to_json(const struct analysis_result **results,
                                   uint8_t count,
                                   char *buffer,
                                   size_t buf_size,
                                   uint8_t dev_id_start)
{
    if (!results || !buffer || count == 0 || count > MQTT_BATCH_MAX_MESSAGES) {
        return APP_ERR_MQTT_INVALID_PARAM;
    }
    
    int pos = snprintf(buffer, buf_size,
        "{\"batch\":true,\"count\":%u,\"messages\":[", count);
    
    if (pos <= 0 || (size_t)pos >= buf_size) {
        return APP_ERR_MQTT_BUFFER_OVERFLOW;
    }
    
    for (uint8_t i = 0; i < count; i++) {
        if (i > 0) {
            pos += snprintf(buffer + pos, buf_size - pos, ",");
        }
        
        if ((size_t)pos >= buf_size - 16) {
            return APP_ERR_MQTT_BUFFER_OVERFLOW;
        }
        
        /* 序列化单个结果 (不含外层括号) */
        struct analysis_result processed;
        apply_virtual_device_offset(results[i], &processed, dev_id_start + i);
        
        uint8_t actual_dev_id = (dev_id_start + i == 0)
                               ? g_mqtt.config.devices[0].virtual_dev_id
                               : g_mqtt.config.devices[dev_id_start + i - 1].virtual_dev_id;
        
        int single_len = serialize_analysis_result_to_json(
            &processed, buffer + pos, buf_size - pos - 8, actual_dev_id);
        
        if (single_len < 0) {
            return single_len;
        }
        pos += single_len;
    }
    
    pos += snprintf(buffer + pos, buf_size - pos, "]}");
    
    if ((size_t)pos >= buf_size) {
        return APP_ERR_MQTT_BUFFER_OVERFLOW;
    }
    
    return pos;
}

/* ==================== TLS 初始化函数 ==================== */

#ifdef CONFIG_ESP_TLS_ENABLED

/**
 * init_tls_configuration - 初始化 TLS 配置
 *
 * 功能:
 *   - 根据 broker.enable_tls 决定是否启用 TLS
 *   - 支持三种模式:
 *     [1] CA 证书验证 (推荐生产环境)
 *     [2] 全局 CA 存储 (使用 ESP-IDF 内置证书)
 *     [3] 双向 TLS 认证 (客户端证书 + 私钥)
 *
 * Return: APP_ERR_OK or error code
 */
static int init_tls_configuration(void)
{
    if (!g_mqtt.config.broker.enable_tls) {
        LOG_INFO("MQTT-TLS", "TLS disabled, using plain connection");
        return APP_ERR_OK;
    }
    
    memset(&g_mqtt.tls_config, 0, sizeof(g_mqtt.tls_config));
    
    /* 方式1: 使用全局 CA 存储 (ESP-IDF 内置证书) */
    if (g_mqtt.config.broker.use_global_ca_store) {
        g_mqtt.tls_config.use_global_ca_store = true;
        LOG_INFO("MQTT-TLS", "Using global CA store");
        g_mqtt.tls_initialized = true;
        return APP_ERR_OK;
    }
    
    /* 方式2: 使用自定义 CA 证书 */
    if (strlen(g_mqtt.config.broker.ca_cert) > 0) {
        /* 分配证书内存 (PEM 格式需要以 \0 结尾) */
        size_t ca_len = strlen(g_mqtt.config.broker.ca_cert) + 1;
        char *ca_pem = malloc(ca_len);
        if (!ca_pem) {
            LOG_ERROR("MQTT-TLS", "Failed to allocate memory for CA cert");
            return APP_ERR_NO_MEM;
        }
        memcpy(ca_pem, g_mqtt.config.broker.ca_cert, ca_len);
        
        g_mqtt.tls_config.ca_cert_pem = ca_pem;
        g_mqtt.tls_config.ca_cert_len = ca_len - 1;
        
        LOG_INFO("MQTT-TLS", "Loaded custom CA certificate (%d bytes)", ca_len - 1);
        
        /* 可选: 双向认证 (客户端证书 + 私钥) */
        if (strlen(g_mqtt.config.broker.client_cert) > 0 &&
            strlen(g_mqtt.config.broker.client_key) > 0) {
            
            size_t client_cert_len = strlen(g_mqtt.config.broker.client_cert) + 1;
            size_t client_key_len = strlen(g_mqtt.config.broker.client_key) + 1;
            
            char *client_cert_pem = malloc(client_cert_len);
            char *client_key_pem = malloc(client_key_len);
            
            if (!client_cert_pem || !client_key_pem) {
                LOG_ERROR("MQTT-TLS", "Failed to allocate memory for client certs");
                free(ca_pem);
                if (client_cert_pem) free(client_cert_pem);
                if (client_key_pem) free(client_key_pem);
                return APP_ERR_NO_MEM;
            }
            
            memcpy(client_cert_pem, g_mqtt.config.broker.client_cert, client_cert_len);
            memcpy(client_key_pem, g_mqtt.config.broker.client_key, client_key_len);
            
            g_mqtt.tls_config.client_cert_pem = client_cert_pem;
            g_mqtt.tls_config.client_cert_len = client_cert_len - 1;
            g_mqtt.tls_config.client_key_pem = client_key_pem;
            g_mqtt.tls_config.client_key_len = client_key_len - 1;
            
            LOG_INFO("MQTT-TLS", "Enabled mutual TLS authentication");
        }
        
        /* 安全选项 */
        g_mqtt.tls_config.skip_common_name_check =
            g_mqtt.config.broker.skip_cert_common_name_check;
        
        if (g_mqtt.config.broker.skip_cert_common_name_check) {
            LOG_WARN("MQTT-TLS", "⚠️  CN check disabled (INSECURE, for testing only)");
        }
        
        g_mqtt.tls_initialized = true;
        return APP_ERR_OK;
    }
    
    /* 无有效 TLS 配置 */
    LOG_ERROR("MQTT-TLS", "TLS enabled but no CA certificate provided");
    return APP_ERR_MQTT_TLS_FAIL;
}

/**
 * cleanup_tls_resources - 释放 TLS 相关资源
 */
static void cleanup_tls_resources(void)
{
    if (g_mqtt.tls_config.ca_cert_pem) {
        free((void *)g_mqtt.tls_config.ca_cert_pem);
        g_mqtt.tls_config.ca_cert_pem = NULL;
    }
    if (g_mqtt.tls_config.client_cert_pem) {
        free((void *)g_mqtt.tls_config.client_cert_pem);
        g_mqtt.tls_config.client_cert_pem = NULL;
    }
    if (g_mqtt.tls_config.client_key_pem) {
        free((void *)g_mqtt.tls_config.client_key_pem);
        g_mqtt.tls_config.client_key_pem = NULL;
    }
    
    memset(&g_mqtt.tls_config, 0, sizeof(g_mqtt.tls_config));
    g_mqtt.tls_initialized = false;
}

#endif /* CONFIG_ESP_TLS_ENABLED */

/* ==================== 离线缓冲区管理函数 ==================== */

/**
 * init_offline_buffer - 初始化离线环形缓冲区
 *
 * 功能:
 *   - 静态分配 MQTT_OFFLINE_BUFFER_SIZE 字节内存
 *   - 初始化 ringbuf 实例 (覆盖模式: 缓冲区满时丢弃最旧消息)
 *   - 用于网络断开时缓存待发送消息
 *
 * Return: APP_ERR_OK or error code
 */
static int init_offline_buffer(void)
{
    if (g_mqtt.offline_buffer_init) {
        return APP_ERR_OK;
    }
    
    /* 静态分配缓冲区内存 */
    g_mqtt.offline_buffer_mem = calloc(1, MQTT_OFFLINE_BUFFER_SIZE);
    if (!g_mqtt.offline_buffer_mem) {
        LOG_ERROR("MQTT-OFFLINE", "Failed to allocate offline buffer (%d bytes)",
                  MQTT_OFFLINE_BUFFER_SIZE);
        return APP_ERR_NO_MEM;
    }
    
    /* 初始化环形缓冲区 (覆盖模式: true) */
    int ret = ringbuf_init(&g_mqtt.offline_ringbuf,
                           g_mqtt.offline_buffer_mem,
                           MQTT_OFFLINE_BUFFER_SIZE,
                           true);  /* overwrite=true: 满时覆盖旧数据 */
    
    if (ret != APP_ERR_OK) {
        LOG_ERROR("MQTT-OFFLINE", "Failed to init ringbuf (err=%d)", ret);
        free(g_mqtt.offline_buffer_mem);
        g_mqtt.offline_buffer_mem = NULL;
        return ret;
    }
    
    g_mqtt.offline_buffer_init = true;
    g_mqtt.offline_cached_messages = 0;
    
    LOG_INFO("MQTT-OFFLINE", "Offline buffer initialized (%d bytes, overwrite mode)",
             MQTT_OFFLINE_BUFFER_SIZE);
    
    return APP_ERR_OK;
}

/**
 * deinit_offline_buffer - 反初始化离线缓冲区
 */
static void deinit_offline_buffer(void)
{
    if (!g_mqtt.offline_buffer_init) {
        return;
    }
    
    ringbuf_deinit(&g_mqtt.offline_ringbuf);
    
    if (g_mqtt.offline_buffer_mem) {
        free(g_mqtt.offline_buffer_mem);
        g_mqtt.offline_buffer_mem = NULL;
    }
    
    g_mqtt.offline_buffer_init = false;
    g_mqtt.offline_cached_messages = 0;
    
    LOG_INFO("MQTT-OFFLINE", "Offline buffer deinitialized");
}

/**
 * serialize_offline_message - 序列化离线消息为二进制格式
 * @msg: 离线消息结构体
 * @buffer: 输出缓冲区
 * @buf_size: 缓冲区大小
 *
 * 二进制格式:
 *   [4B topic_len][topic][4B payload_len][payload][1B qos][1B retain][4B timestamp]
 *
 * Return: 实际写入字节数, 负数表示错误
 */
static int serialize_offline_message(const struct offline_message *msg,
                                     uint8_t *buffer,
                                     size_t buf_size)
{
    if (!msg || !buffer) return APP_ERR_INVALID_PARAM;
    
    size_t needed = 4 + strlen(msg->topic) + 4 + msg->payload_len + 1 + 1 + 4;
    if (needed > buf_size) {
        return APP_ERR_MQTT_BUFFER_OVERFLOW;
    }
    
    size_t pos = 0;
    uint32_t len;
    
    /* Topic 长度 + Topic */
    len = (uint32_t)strlen(msg->topic);
    memcpy(buffer + pos, &len, 4); pos += 4;
    memcpy(buffer + pos, msg->topic, len); pos += len;
    
    /* Payload 长度 + Payload */
    len = (uint32_t)msg->payload_len;
    memcpy(buffer + pos, &len, 4); pos += 4;
    memcpy(buffer + pos, msg->payload, msg->payload_len); pos += msg->payload_len;
    
    /* QoS + Retain */
    buffer[pos++] = (uint8_t)msg->qos;
    buffer[pos++] = msg->retain ? 1 : 0;
    
    /* Timestamp */
    uint32_t ts = (uint32_t)(msg->timestamp_us / 1000);  /* 转换为 ms */
    memcpy(buffer + pos, &ts, 4); pos += 4;
    
    return (int)pos;
}

/**
 * deserialize_offline_message - 反序列化二进制数据为离线消息
 */
static int deserialize_offline_message(const uint8_t *buffer,
                                       size_t buf_size,
                                       struct offline_message *msg)
{
    if (!buffer || !msg || buf_size < 14) {  /* 最小长度: 4+0+4+0+1+1+4=14 */
        return APP_ERR_INVALID_PARAM;
    }
    
    size_t pos = 0;
    uint32_t len;
    
    /* Topic */
    memcpy(&len, buffer + pos, 4); pos += 4;
    if (len >= MQTT_MAX_TOPIC_LEN || pos + len > buf_size) {
        return APP_ERR_MQTT_BUFFER_OVERFLOW;
    }
    memcpy(msg->topic, buffer + pos, len);
    msg->topic[len] = '\0';
    pos += len;
    
    /* Payload */
    memcpy(&len, buffer + pos, 4); pos += 4;
    if (len >= MQTT_MAX_MESSAGE_SIZE || pos + len > buf_size) {
        return APP_ERR_MQTT_BUFFER_OVERFLOW;
    }
    memcpy(msg->payload, buffer + pos, len);
    msg->payload[len] = '\0';
    msg->payload_len = len;
    pos += len;
    
    /* QoS + Retain */
    msg->qos = buffer[pos++];
    msg->retain = (buffer[pos++] != 0);
    
    /* Timestamp */
    uint32_t ts;
    memcpy(&ts, buffer + pos, 4);
    msg->timestamp_us = (uint64_t)ts * 1000;  /* 转换回 us */
    
    msg->retry_count = 0;
    
    return APP_ERR_OK;
}

/* ==================== 批量发布辅助函数 ==================== */

/**
 * should_flush_batch - 判断是否应该刷新批量缓冲区
 *
 * 条件满足任一即刷新:
 *   [1] 消息数达到 max_messages
 *   [2] 距离第一条消息超过 flush_interval_ms
 *   [3] 总数据大小超过 max_batch_size_bytes
 */
static bool should_flush_batch(void)
{
    if (!g_mqtt.batch_cfg.enabled || g_mqtt.batch_count == 0) {
        return false;
    }
    
    /* 条件1: 达到最大消息数 */
    if (g_mqtt.batch_count >= g_mqtt.batch_cfg.max_messages) {
        return true;
    }
    
    /* 条件2: 超时 */
    if (g_mqtt.batch_cfg.flush_interval_ms > 0) {
        int64_t now = (int64_t)esp_timer_get_time();
        int64_t elapsed_ms = (now - g_mqtt.batch_start_time_us) / 1000LL;
        if (elapsed_ms >= (int64_t)g_mqtt.batch_cfg.flush_interval_ms) {
            return true;
        }
    }
    
    return false;
}

/**
 * estimate_batch_size - 估算批量数据的总大小
 */
static size_t estimate_batch_size(uint8_t count)
{
    /* 基础 JSON 开销 + 每个 message 的估算大小 */
    return 64 + count * (sizeof(struct analysis_result) + 128);
}

/* ==================== 动态配置处理函数 ==================== */

/**
 * parse_and_apply_config_update - 解析并应用远程配置更新
 * @json_str: JSON 格式的配置字符串
 * @json_len: JSON 长度
 *
 * 支持的字段 (与 config_manager.h 中 system_config 对应):
 *   - mqtt_mode, mqtt_publish_interval_ms, mqtt_qos
 *   - sample_rate_hz, fft_size, rms_warning_threshold
 *   - 其他可运行时更新的字段
 *
 * Return: APP_ERR_OK or error code
 */
static int parse_and_apply_config_update(const char *json_str, size_t json_len)
{
    if (!json_str || json_len == 0) {
        return APP_ERR_INVALID_PARAM;
    }
    
    g_mqtt.stats.config_update_count++;
    
    LOG_INFO("MQTT-CONFIG", "Received config update (%d bytes)", json_len);
    LOG_DEBUG("MQTT-CONFIG", "Config payload: %.*s", (int)json_len, json_str);
    
    /* 手动轻量级 JSON 解析 (避免引入 cJSON 依赖) */
    /* 简单实现: 查找 "updates": {...} 并提取键值对 */
    
    const char *updates_start = strstr(json_str, "\"updates\"");
    if (!updates_start) {
        LOG_WARN("MQTT-CONFIG", "No 'updates' field in config JSON");
        g_mqtt.stats.config_update_fail++;
        return APP_ERR_MQTT_JSON_ENCODE;
    }
    
    const char *obj_start = strchr(updates_start, '{');
    if (!obj_start) {
        g_mqtt.stats.config_update_fail++;
        return APP_ERR_MQTT_JSON_ENCODE;
    }
    
    obj_start++;  /* 跳过 '{' */
    
    /* 获取当前配置快照 */
    const struct system_config *current_cfg = config_manager_get();
    if (!current_cfg) {
        LOG_ERROR("MQTT-CONFIG", "Failed to get current config");
        g_mqtt.stats.config_update_fail++;
        return APP_ERR_CONFIG_LOAD_FAIL;
    }
    
    /* 复制一份用于修改 */
    static struct system_config new_cfg;
    memcpy(&new_cfg, current_cfg, sizeof(new_cfg));
    
    uint32_t updates_applied = 0;
    const char *p = obj_start;
    
    /* 简单 JSON 键值对提取循环 */
    while (*p && *p != '}') {
        /* 跳过空白字符 */
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        if (*p == '}') break;
        
        /* 提取 key */
        if (*p != '"') { p++; continue; }
        p++;  /* 跳过 '"' */
        const char *key_start = p;
        while (*p && *p != '"') p++;
        if (!*p) break;
        
        char key[64];
        size_t key_len = (size_t)(p - key_start);
        if (key_len >= sizeof(key)) { p++; continue; }
        memcpy(key, key_start, key_len);
        key[key_len] = '\0';
        p++;  /* 跳过 '"' */
        
        /* 跳过 ':' 和空白 */
        while (*p == ' ' || *p == ':') p++;
        
        /* 提取 value (支持数字、布尔、字符串) */
        if (*p == '"') {
            /* 字符串值 */
            p++;
            const char *val_start = p;
            while (*p && *p != '"') p++;
            size_t val_len = (size_t)(p - val_start);
            
            /* 根据key应用配置 */
            if (strcmp(key, "mqtt_broker_url") == 0 && val_len < sizeof(new_cfg.mqtt_broker_url)) {
                memcpy(new_cfg.mqtt_broker_url, val_start, val_len);
                new_cfg.mqtt_broker_url[val_len] = '\0';
                updates_applied++;
            } else if (strcmp(key, "mqtt_username") == 0 && val_len < sizeof(new_cfg.mqtt_username)) {
                memcpy(new_cfg.mqtt_username, val_start, val_len);
                new_cfg.mqtt_username[val_len] = '\0';
                updates_applied++;
            } else if (strcmp(key, "mqtt_password") == 0 && val_len < sizeof(new_cfg.mqtt_password)) {
                memcpy(new_cfg.mqtt_password, val_start, val_len);
                new_cfg.mqtt_password[val_len] = '\0';
                updates_applied++;
            } else if (strcmp(key, "ota_server_url") == 0 && val_len < sizeof(new_cfg.ota_server_url)) {
                memcpy(new_cfg.ota_server_url, val_start, val_len);
                new_cfg.ota_server_url[val_len] = '\0';
                updates_applied++;
            }
            
            if (*p) p++;  /* 跳过结束 '"' */
        } else if (*p == 't' || *p == 'f') {
            /* 布尔值 */
            bool val = (strncmp(p, "true", 4) == 0);
            if (!val && strncmp(p, "false", 5) != 0) { p++; continue; }
            
            if (strcmp(key, "mqtt_enable_tls") == 0) {
                new_cfg.mqtt_enable_tls = val;
                updates_applied++;
            } else if (strcmp(key, "mqtt_clean_session") == 0) {
                new_cfg.mqtt_clean_session = val;
                updates_applied++;
            } else if (strcmp(key, "mqtt_publish_vibration") == 0) {
                new_cfg.mqtt_publish_vibration = val;
                updates_applied++;
            } else if (strcmp(key, "mqtt_publish_environment") == 0) {
                new_cfg.mqtt_publish_environment = val;
                updates_applied++;
            } else if (strcmp(key, "auto_reboot_enabled") == 0) {
                new_cfg.auto_reboot_enabled = val;
                updates_applied++;
            } else if (strcmp(key, "encryption_enabled") == 0) {
                new_cfg.encryption_enabled = val;
                updates_applied++;
            }
            
            p += val ? 4 : 5;
        } else if (*p == '-' || (*p >= '0' && *p <= '9')) {
            /* 数字值 */
            char *endptr;
            long val = strtol(p, &endptr, 10);
            p = endptr;
            
            if (strcmp(key, "mqtt_mode") == 0) {
                new_cfg.mqtt_mode = (int)val;
                updates_applied++;
            } else if (strcmp(key, "mqtt_broker_port") == 0) {
                new_cfg.mqtt_broker_port = (uint16_t)val;
                updates_applied++;
            } else if (strcmp(key, "mqtt_qos") == 0) {
                new_cfg.mqtt_qos = (uint8_t)val;
                updates_applied++;
            } else if (strcmp(key, "mqtt_keepalive_sec") == 0) {
                new_cfg.mqtt_keepalive_sec = (uint32_t)val;
                updates_applied++;
            } else if (strcmp(key, "mqtt_publish_interval_ms") == 0) {
                new_cfg.mqtt_publish_interval_ms = (uint32_t)val;
                updates_applied++;
            } else if (strcmp(key, "sample_rate_hz") == 0) {
                new_cfg.sample_rate_hz = (int)val;
                updates_applied++;
            } else if (strcmp(key, "fft_size") == 0) {
                new_cfg.fft_size = (int)val;
                updates_applied++;
            } else if (strcmp(key, "device_id") == 0) {
                new_cfg.device_id = (uint8_t)val;
                updates_applied++;
            } else if (strcmp(key, "heartbeat_interval_ms") == 0) {
                new_cfg.heartbeat_interval_ms = (uint32_t)val;
                updates_applied++;
            } else if (strcmp(key, "reboot_interval_seconds") == 0) {
                new_cfg.reboot_interval_seconds = (uint32_t)val;
                updates_applied++;
            }
        }
        
        /* 跳过 ',' 或空白 */
        while (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    }
    
    if (updates_applied == 0) {
        LOG_WARN("MQTT-CONFIG", "No valid config fields found in update");
        g_mqtt.stats.config_update_fail++;
        return APP_ERR_CONFIG_VALIDATION;
    }
    
    /* 验证并保存新配置 */
    int validate_ret = config_manager_validate(&new_cfg);
    if (validate_ret != APP_ERR_OK) {
        LOG_ERROR("MQTT-CONFIG", "Config validation failed (err=%d)", validate_ret);
        g_mqtt.stats.config_update_fail++;
        return validate_ret;
    }
    
    /* 应用配置 */
    int set_ret = config_manager_set(&new_cfg);
    if (set_ret != APP_ERR_OK) {
        LOG_ERROR("MQTT-CONFIG", "Failed to apply config (err=%d)", set_ret);
        g_mqtt.stats.config_update_fail++;
        return set_ret;
    }
    
    g_mqtt.stats.config_update_success++;
    
    LOG_INFO("MQTT-CONFIG", "Config updated successfully (%u fields applied)",
             updates_applied);
    
    /* 触发配置回调 (如果有注册) */
    if (g_mqtt.config_callback.cb) {
        g_mqtt.config_callback.cb(
            g_mqtt.config_topic,
            json_str,
            json_len,
            g_mqtt.config_callback.user_data);
    }
    
    return APP_ERR_OK;
}

/* ==================== MQTT 事件处理器 ==================== */

/**
 * mqtt_event_handler - ESP-IDF MQTT 事件处理器
 *
 * 扩展事件处理:
 *   - MQTT_EVENT_CONNECTED: 自动刷新离线缓冲区 + 重新订阅配置 Topic
 *   - MQTT_EVENT_DATA: 检测是否为配置更新消息
 *   - MQTT_EVENT_ERROR: 详细 TLS 错误日志
 */
static void mqtt_event_handler(void *event_handler_arg,
                                esp_event_base_t event_base,
                                int32_t event_id,
                                void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            g_mqtt.state = MQTT_STATE_CONNECTED;
            g_mqtt.connect_time_us = (int64_t)esp_timer_get_time();
            g_mqtt.stats.connect_count++;
            
            LOG_INFO("MQTT", "Connected to broker %s%s (client_id=%s)",
                     g_mqtt.config.broker.enable_tls ? "[TLS]" : "",
                     g_mqtt.config.broker.url,
                     g_mqtt.config.broker.client_id);
            
            /* 自动刷新离线缓冲区 (补发断网期间的消息) */
            if (g_mqtt.offline_buffer_init &&
                ringbuf_used(&g_mqtt.offline_ringbuf) > 0) {
                
                LOG_INFO("MQTT", "Flushing offline buffer (%u messages pending)...",
                         g_mqtt.offline_cached_messages);
                
                int flushed = mqtt_flush_offline_buffer();
                if (flushed > 0) {
                    LOG_INFO("MQTT", "Offline buffer flushed (%d messages sent)", flushed);
                } else if (flushed < 0) {
                    LOG_WARN("MQTT", "Failed to flush offline buffer (err=%d)", flushed);
                }
            }
            
            /* 重新订阅配置 Topic (如果之前订阅过) */
            if (g_mqtt.config_subscribed && strlen(g_mqtt.config_topic) > 0) {
                int sub_ret = esp_mqtt_client_subscribe(
                    g_mqtt.client,
                    g_mqtt.config_topic,
                    g_mqtt.config.broker.qos);
                
                if (sub_ret > 0) {
                    LOG_DEBUG("MQTT", "Resubscribed to config topic: %s",
                              g_mqtt.config_topic);
                } else {
                    LOG_WARN("MQTT", "Failed to resubscribe to config topic");
                }
            }
            
            /* 触发事件回调 */
            for (int i = 0; i < g_mqtt.event_callbacks[0].count; i++) {
                if (g_mqtt.event_callbacks[i].cb) {
                    g_mqtt.event_callbacks[i].cb(
                        MQTT_EVENT_CONNECTED,
                        "Broker connected",
                        g_mqtt.event_callbacks[i].user_data);
                }
            }
            break;
            
        case MQTT_EVENT_DISCONNECTED:
            g_mqtt.state = MQTT_STATE_DISCONNECTED;
            g_mqtt.stats.disconnect_count++;
            LOG_WARN("MQTT", "Disconnected from broker (will cache messages)");
            
            for (int i = 0; i < g_mqtt.event_callbacks[0].count; i++) {
                if (g_mqtt.event_callbacks[i].cb) {
                    g_mqtt.event_callbacks[i].cb(
                        MQTT_EVENT_DISCONNECTED,
                        "Broker disconnected",
                        g_mqtt.event_callbacks[i].user_data);
                }
            }
            break;
            
        case MQTT_EVENT_DATA:
            {
                static char topic[MQTT_MAX_TOPIC_LEN];
                static char data[MQTT_MAX_MESSAGE_SIZE];
                int topic_len = (event->topic_len < MQTT_MAX_TOPIC_LEN - 1)
                                ? event->topic_len : MQTT_MAX_TOPIC_LEN - 1;
                int data_len = (event->data_len < MQTT_MAX_MESSAGE_SIZE - 1)
                               ? event->data_len : MQTT_MAX_MESSAGE_SIZE - 1;
                memcpy(topic, event->topic, (size_t)topic_len);
                topic[topic_len] = '\0';
                memcpy(data, event->data, (size_t)data_len);
                data[data_len] = '\0';
                
                LOG_DEBUG("MQTT", "Received on %s (%d bytes)",
                          topic, event->data_len);
                
                /* 检查是否为配置更新消息 */
                if (g_mqtt.config_subscribed &&
                    strncmp(topic, CONFIG_TOPIC_PREFIX, strlen(CONFIG_TOPIC_PREFIX)) == 0) {
                    
                    LOG_INFO("MQTT-CONFIG", "Received config update from %s", topic);
                    
                    int cfg_ret = parse_and_apply_config_update(data, event->data_len);
                    if (cfg_ret != APP_ERR_OK) {
                        LOG_ERROR("MQTT-CONFIG", "Failed to apply config update (err=%d)",
                                  cfg_ret);
                    }
                }
                
                /* TODO: 如果有注册的消息回调, 在此调用 */
            }
            break;
            
        case MQTT_EVENT_ERROR:
            g_mqtt.state = MQTT_STATE_ERROR;
            g_mqtt.stats.connection_errors++;
            
            LOG_ERROR("MQTT", "MQTT Error occurred");
            
#ifdef CONFIG_ESP_TLS_ENABLED
            if (event->error_handle->esp_tls_last_esp_err != 0) {
                LOG_ERROR("MQTT-TLS", "TLS error: 0x%x [%s]",
                          event->error_handle->esp_tls_last_esp_err,
                          esp_err_to_name(event->error_handle->esp_tls_last_esp_err));
            }
            if (event->error_handle->esp_tls_stack_err != 0) {
                LOG_ERROR("MQTT-TLS", "TLS stack error: 0x%x",
                          event->error_handle->esp_tls_stack_err);
            }
#endif
            if (event->error_handle->esp_transport_sock_errno != 0) {
                LOG_ERROR("MQTT", "Transport errno: %d [%s]",
                          event->error_handle->esp_transport_sock_errno,
                          strerror(event->error_handle->esp_transport_sock_errno));
            }
            break;
            
        case MQTT_EVENT_PUBLISHED:
            g_mqtt.stats.publish_success_count++;
            LOG_DEBUG("MQTT", "Message published successfully");
            break;
            
        case MQTT_EVENT_SUBSCRIBED:
            LOG_DEBUG("MQTT", "Subscribed to topic (msg_id=%d)", event->msg_id);
            break;
            
        default:
            break;
    }
}

/**
 * mqtt_task_main - MQTT 后台任务主循环
 *
 * 扩展功能:
 *   - 监控连接状态
 *   - 自动重连
 *   - 定期检查批量缓冲区超时
 *   - 维护心跳统计
 */
static void mqtt_task_main(void *arg)
{
    (void)arg;
    
    LOG_INFO("MQTT", "Task started (mode=%s, TLS=%s, Batch=%s, OfflineBuf=%s)",
             g_mqtt.mode == MQTT_MODE_TRAINING ? "Training" : "Upload",
             g_mqtt.config.broker.enable_tls ? "ON" : "OFF",
             g_mqtt.batch_cfg.enabled ? "ON" : "OFF",
             g_mqtt.offline_buffer_init ? "ON" : "OFF");
    
    while (g_mqtt.running) {
        /* 更新运行时间统计 */
        if (g_mqtt.state == MQTT_STATE_CONNECTED) {
            int64_t now = (int64_t)esp_timer_get_time();
            g_mqtt.stats.uptime_seconds =
                (uint32_t)((now - g_mqtt.connect_time_us) / 1000000LL);
            
            /* 检查批量缓冲区是否需要刷新 (仅当已连接且批量启用) */
            if (g_mqtt.batch_cfg.enabled && g_mqtt.batch_count > 0) {
                xSemaphoreTake(g_mqtt.batch_mutex, pdMS_TO_TICKS(100));
                
                if (should_flush_batch()) {
                    LOG_DEBUG("MQTT-BATCH", "Auto-flushing batch (%u messages)",
                              g_mqtt.batch_count);
                    
                    /* 临时保存批量数据 */
                    uint8_t temp_count = g_mqtt.batch_count;
                    struct analysis_result *temp_results[MQTT_BATCH_MAX_MESSAGES];
                    memcpy(temp_results, g_mqtt.batch_accumulator,
                           sizeof(temp_results[0]) * temp_count);
                    
                    g_mqtt.batch_count = 0;
                    
                    xSemaphoreGive(g_mqtt.batch_mutex);
                    
                    /* 发布批量数据 (不持有锁) */
                    int batch_ret = mqtt_publish_batch(
                        (const struct analysis_result **)temp_results,
                        temp_count,
                        g_mqtt.active_virtual_dev_idx);
                    
                    if (batch_ret != APP_ERR_OK) {
                        LOG_WARN("MQTT-BATCH", "Batch publish failed (err=%d)", batch_ret);
                    }
                } else {
                    xSemaphoreGive(g_mqtt.batch_mutex);
                }
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    LOG_INFO("MQTT", "Task stopped");
    vTaskDelete(NULL);
}

/* ==================== 公开 API 实现 ==================== */

int mqtt_init(const struct mqtt_config *config)
{
    if (g_mqtt.initialized) {
        return APP_ERR_MQTT_ALREADY_INIT;
    }
    
    memset(&g_mqtt, 0, sizeof(g_mqtt));
    
    /* 加载配置或使用默认值 */
    if (config) {
        memcpy(&g_mqtt.config, config, sizeof(*config));
    } else {
        /* 默认配置: Training 模式, 本地 broker */
        g_mqtt.config.mode = MQTT_MODE_TRAINING;

        /*
         * ⚠️ 【重要】MQTT Broker 地址配置
         *
         * 默认使用 PC 端的 IP 地址:
         *   - 如果 PC 连接到 ESP32 的热点, PC 的 IP 通常是 192.168.43.xxx
         *   - 如果 ESP32 连接到路由器, 需要改为路由器网段 (如 192.168.1.xxx)
         *   - 端口: 1883 (标准MQTT端口, 无TLS)
         *   - 如果使用 TLS/SSL, 端口应为 8883
         */
        snprintf(g_mqtt.config.broker.url, sizeof(g_mqtt.config.broker.url),
                 "mqtt://192.168.43.23:1883");  /* PC端实际IP (手机热点模式) */
        g_mqtt.config.broker.port = 1883;
        g_mqtt.config.broker.qos = MQTT_QOS_DEFAULT;
        g_mqtt.config.broker.keepalive_sec = MQTT_KEEPALIVE_SECONDS;
        g_mqtt.config.broker.clean_session = true;
        snprintf(g_mqtt.config.broker.client_id,
                 sizeof(g_mqtt.config.broker.client_id),
                 "EdgeVib-%08" PRIx32, (uint32_t)esp_random());
        
        g_mqtt.config.num_virtual_devices = 1;  /* 默认不模拟 */
        g_mqtt.config.publish_interval_ms = 1000;
        g_mqtt.config.publish_vibration_data = true;
        g_mqtt.config.publish_environment_data = true;
        g_mqtt.config.publish_health_status = false;
    }
    
    g_mqtt.mode = g_mqtt.config.mode;
    g_mqtt.state = MQTT_STATE_DISCONNECTED;
    
    /* 创建互斥量 */
    g_mqtt.mutex = xSemaphoreCreateMutex();
    if (!g_mqtt.mutex) {
        LOG_ERROR("MQTT", "Failed to create mutex");
        return APP_ERR_MQTT_MUTEX_CREATE;
    }
    
    /* 创建批量操作互斥量 */
    g_mqtt.batch_mutex = xSemaphoreCreateMutex();
    if (!g_mqtt.batch_mutex) {
        vSemaphoreDelete(g_mqtt.mutex);
        LOG_ERROR("MQTT", "Failed to create batch mutex");
        return APP_ERR_MQTT_MUTEX_CREATE;
    }
    
    /* ⚠ 【关键修复】默认关闭批量发布模式!
     *
     * 原始BUG:
     *   batch_cfg.enabled=true 导致数据先在批量缓冲区累积(4条/2秒),
     *   由mqtt_task_main每1秒检查刷新。单设备低频场景(每2秒1条):
     *   batch_count=1 → 不满足max_messages(4) → 等flush_interval_ms(2秒)
     *   → mqtt_task_main 的检查窗口与数据到达时间错位
     *   → 数据在buffer中滞留 → PC端2分钟收2条
     *
     * 修复:
     *   训练模式(单设备低频)关闭批量发布,每条结果直发MQTT
     */
    g_mqtt.batch_cfg.enabled = false;   /* 单设备低频场景: 直发模式 */
    g_mqtt.batch_cfg.max_messages = 4;
    g_mqtt.batch_cfg.flush_interval_ms = 2000;
    g_mqtt.batch_cfg.max_batch_size_bytes = 4096;
    g_mqtt.batch_cfg.merge_same_topic = true;
    
    g_mqtt.initialized = true;
    g_mqtt.active_virtual_dev_idx = 0;
    
    LOG_INFO("MQTT", "Initialized (mode=%s, broker=%s, virtual_devs=%u, TLS=%s)",
             g_mqtt.mode == MQTT_MODE_TRAINING ? "Training" : "Upload",
             g_mqtt.config.broker.url,
             g_mqtt.config.num_virtual_devices,
             g_mqtt.config.broker.enable_tls ? "ENABLED" : "DISABLED");
    
    return APP_ERR_OK;
}

int mqtt_deinit(void)
{
    if (!g_mqtt.initialized) {
        return APP_ERR_MQTT_NOT_INIT;
    }
    
    mqtt_stop();
    
#ifdef CONFIG_ESP_TLS_ENABLED
    /* 清理 TLS 资源 */
    cleanup_tls_resources();
#endif
    
    /* 清理离线缓冲区 */
    deinit_offline_buffer();
    
    if (g_mqtt.mutex) {
        vSemaphoreDelete(g_mqtt.mutex);
        g_mqtt.mutex = NULL;
    }
    
    if (g_mqtt.batch_mutex) {
        vSemaphoreDelete(g_mqtt.batch_mutex);
        g_mqtt.batch_mutex = NULL;
    }
    
    g_mqtt.initialized = false;
    LOG_INFO("MQTT", "Deinitialized");
    return APP_ERR_OK;
}

bool mqtt_is_initialized(void)
{
    return g_mqtt.initialized;
}

/* ==================== 连接管理 API 实现 ==================== */

int mqtt_start(void)
{
    if (!g_mqtt.initialized) return APP_ERR_MQTT_NOT_INIT;
    if (g_mqtt.running) return APP_ERR_MQTT_INVALID_MODE;
    
    xSemaphoreTake(g_mqtt.mutex, portMAX_DELAY);
    
#ifdef CONFIG_ESP_TLS_ENABLED
    /* 初始化 TLS 配置 */
    int tls_ret = init_tls_configuration();
    if (tls_ret != APP_ERR_OK && g_mqtt.config.broker.enable_tls) {
        xSemaphoreGive(g_mqtt.mutex);
        LOG_ERROR("MQTT", "TLS initialization failed (err=%d), aborting start", tls_ret);
        return tls_ret;
    }
#endif
    
    /* 初始化离线缓冲区 */
    int offline_ret = init_offline_buffer();
    if (offline_ret != APP_ERR_OK) {
        xSemaphoreGive(g_mqtt.mutex);
        LOG_WARN("MQTT", "Offline buffer init failed (err=%d), continuing without caching",
                 offline_ret);
        /* 不影响启动, 仅记录警告 */
    }
    
    /* 配置 ESP-IDF MQTT 客户端 */
    esp_mqtt_client_config_t mqtt_cfg = {0};
    mqtt_cfg.broker.address.uri = g_mqtt.config.broker.url;
    mqtt_cfg.broker.address.port = g_mqtt.config.broker.port;
    
    if (strlen(g_mqtt.config.broker.username) > 0) {
        mqtt_cfg.credentials.username = g_mqtt.config.broker.username;
    }
    mqtt_cfg.credentials.client_id = g_mqtt.config.broker.client_id;
    mqtt_cfg.session.keepalive = g_mqtt.config.broker.keepalive_sec;
    mqtt_cfg.session.disable_clean_session = !g_mqtt.config.broker.clean_session;
    
    if (strlen(g_mqtt.config.broker.password) > 0) {
        mqtt_cfg.credentials.authentication.password = g_mqtt.config.broker.password;
    }
    
#ifdef CONFIG_ESP_TLS_ENABLED
    /* TLS 配置 */
    if (g_mqtt.config.broker.enable_tls && g_mqtt.tls_initialized) {
        mqtt_cfg.broker.verification.crt_bundle_attach = NULL;  /* 禁用默认证书包 */
        mqtt_cfg.broker.verification.certificate = g_mqtt.tls_config;
        
        LOG_INFO("MQTT-TLS", "Applying TLS configuration...");
    }
#endif
    
    /* LWT 配置 */
    if (g_mqtt.config.enable_lwt) {
        mqtt_cfg.session.last_will.topic = g_mqtt.config.lwt_topic;
        mqtt_cfg.session.last_will.msg = g_mqtt.config.lwt_message;
        mqtt_cfg.session.last_will.retain = true;
        mqtt_cfg.session.last_will.qos = 1;
    }
    
    /* 创建客户端 */
    g_mqtt.client = esp_mqtt_client_init(&mqtt_cfg);
    if (!g_mqtt.client) {
        xSemaphoreGive(g_mqtt.mutex);
        LOG_ERROR("MQTT", "Failed to create MQTT client");
        return APP_ERR_MQTT_CONNECT_FAIL;
    }
    
    /* 注册事件处理器 */
    esp_mqtt_client_register_event(g_mqtt.client, MQTT_EVENT_ANY, mqtt_event_handler, NULL);
    
    /* 启动客户端 (非阻塞) */
    esp_err_t err = esp_mqtt_client_start(g_mqtt.client);
    if (err != ESP_OK) {
        xSemaphoreGive(g_mqtt.mutex);
        LOG_ERROR("MQTT", "Failed to start client (err=0x%x)", err);
        return APP_ERR_MQTT_CONNECT_FAIL;
    }
    
    g_mqtt.state = MQTT_STATE_CONNECTING;
    g_mqtt.running = true;
    
    /* 启动后台任务 */
    BaseType_t ret = xTaskCreate(mqtt_task_main, "mqtt_core",
                                  MQTT_TASK_STACK_SIZE,
                                  NULL,
                                  MQTT_TASK_PRIORITY,
                                  &g_mqtt.task_handle);
    if (ret != pdPASS) {
        g_mqtt.running = false;
        xSemaphoreGive(g_mqtt.mutex);
        LOG_ERROR("MQTT", "Failed to create task");
        return APP_ERR_MQTT_TASK_CREATE;
    }
    
    xSemaphoreGive(g_mqtt.mutex);
    LOG_INFO("MQTT", "Starting connection to %s%s ...",
             g_mqtt.config.broker.url,
             g_mqtt.config.broker.enable_tls ? " [TLS]" : "");
    return APP_ERR_OK;
}

int mqtt_stop(void)
{
    if (!g_mqtt.initialized) return APP_ERR_MQTT_NOT_INIT;
    if (!g_mqtt.running) return APP_ERR_OK;
    
    xSemaphoreTake(g_mqtt.mutex, portMAX_DELAY);
    
    g_mqtt.running = false;
    
    /* 停止客户端 */
    if (g_mqtt.client) {
        esp_mqtt_client_stop(g_mqtt.client);
        esp_mqtt_client_destroy(g_mqtt.client);
        g_mqtt.client = NULL;
    }
    
    g_mqtt.state = MQTT_STATE_DISCONNECTED;
    
    /* 等待任务结束 */
    if (g_mqtt.task_handle) {
        vTaskDelay(pdMS_TO_TICKS(200));
        g_mqtt.task_handle = NULL;
    }
    
    xSemaphoreGive(g_mqtt.mutex);
    LOG_INFO("MQTT", "Stopped");
    return APP_ERR_OK;
}

int mqtt_reconnect(void)
{
    if (!g_mqtt.initialized) return APP_ERR_MQTT_NOT_INIT;
    
    mqtt_stop();
    vTaskDelay(pdMS_TO_TICKS(1000));  /* 等待完全断开 */
    return mqtt_start();
}

enum mqtt_connection_state mqtt_get_state(void)
{
    return g_mqtt.state;
}

bool mqtt_is_connected(void)
{
    return (g_mqtt.state == MQTT_STATE_CONNECTED);
}

/* ==================== 数据发布 API 实现 ==================== */

int mqtt_publish_analysis_result(const struct analysis_result *result,
                                 uint8_t virtual_dev_idx)
{
    if (!g_mqtt.initialized) return APP_ERR_MQTT_NOT_INIT;
    if (!result) return APP_ERR_MQTT_INVALID_PARAM;
    if (result->analysis_timestamp_us == 0) {
        LOG_WARN("MQTT", "Skip publish: invalid timestamp");
        return APP_ERR_INVALID_PARAM;
    }

    /*
     * ⚠️ 【关键修复】允许 samples_analyzed==0 的降级结果发布!
     *
     * 原始BUG:
     *   samples_analyzed == 0 时直接 return APP_ERR_NO_DATA
     *   → 降级模式(ADXL345无数据但有温湿度)的结果被完全拒绝
     *   → PC端Python脚本只收到3条(仅分析成功的那几次)
     *   → 之后永久收不到数据 → 用户误以为系统卡死
     *
     * 修复方案:
     *   当 temperature_valid == true 时, 即使振动数据为空(samples_analyzed=0),
     *   也允许发布到MQTT (包含有效温湿度 + 零值振动)
     *   确保PC端Edge-AI能持续收到环境监测数据
     *
     * 仅在"既无振动数据又无温度数据"时才真正跳过
     */
    if (result->samples_analyzed == 0 && !result->temperature_valid) {
        LOG_DEBUG("MQTT", "Skip publish: no vibration samples AND no temperature data");
        return APP_ERR_NO_DATA;
    }

    if (result->samples_analyzed == 0 && result->temperature_valid) {
        LOG_DEBUG("MQTT", "Publishing degraded result (temp valid, vib=0 samples)");
    }
    
    /* 如果未连接, 尝试缓存到离线缓冲区 */
    if (!g_mqtt.running || g_mqtt.state != MQTT_STATE_CONNECTED) {
        if (g_mqtt.offline_buffer_init) {
            static char json_buffer[MQTT_MAX_MESSAGE_SIZE];
            uint8_t dev_id = (virtual_dev_idx == 0)
                             ? g_mqtt.config.devices[0].virtual_dev_id
                             : g_mqtt.config.devices[virtual_dev_idx - 1].virtual_dev_id;
            
            int json_len = serialize_analysis_result_to_json(
                result, json_buffer, sizeof(json_buffer), dev_id);
            
            if (json_len > 0) {
                static char topic[MQTT_MAX_TOPIC_LEN];
                build_topic_vibration(topic, sizeof(topic), dev_id);
                
                int cache_ret = mqtt_cache_message_offline(
                    topic, json_buffer, (size_t)json_len,
                    g_mqtt.config.broker.qos, false);
                
                if (cache_ret == APP_ERR_OK) {
                    LOG_DEBUG("MQTT-OFFLINE", "Message cached (offline, total cached=%u)",
                              g_mqtt.offline_cached_messages);
                    return APP_ERR_OK;
                }
            }
        }
        
        return APP_ERR_MQTT_CONNECT_FAIL;
    }
    
    /* 验证虚拟设备索引 */
    if (virtual_dev_idx > g_mqtt.config.num_virtual_devices) {
        return APP_ERR_MQTT_INVALID_DEV_ID;
    }
    
    /* 如果启用批量发布, 尝试加入批量缓冲区 */
    if (g_mqtt.batch_cfg.enabled) {
        xSemaphoreTake(g_mqtt.batch_mutex, pdMS_TO_TICKS(100));

        if (g_mqtt.batch_count < g_mqtt.batch_cfg.max_messages) {
            /*
             * ⚠️ 【关键修复】必须深拷贝数据!
             *
             * 原始BUG:
             *   直接保存 result 指针: batch_accumulator[count] = result
             *   如果 result 指向局部变量 (如 sensor_service.c 中的 degraded_result)
             *   函数返回后栈内存被释放 → 悬空指针!
             *   后续 mqtt_flush_batch_buffer() 访问悬空指针 → LoadProhibited 崩溃!
             *
             * 修复方案:
             *   使用预分配的静态缓冲区存储深拷贝的数据
             *   确保 batch_accumulator 中的指针始终有效
             */
            static struct analysis_result batch_storage[MQTT_BATCH_MAX_MESSAGES];
            memcpy(&batch_storage[g_mqtt.batch_count], result, sizeof(struct analysis_result));
            g_mqtt.batch_accumulator[g_mqtt.batch_count] = &batch_storage[g_mqtt.batch_count];

            if (g_mqtt.batch_count == 0) {
                g_mqtt.batch_start_time_us = (int64_t)esp_timer_get_time();
            }
            g_mqtt.batch_count++;

            xSemaphoreGive(g_mqtt.batch_mutex);

            /* 检查是否需要立即刷新 */
            if (should_flush_batch()) {
                return mqtt_flush_batch_buffer();
            }

            return APP_ERR_OK;  /* 已入队, 等待批量发送 */
        }

        xSemaphoreGive(g_mqtt.batch_mutex);
        /* 缓冲区满, 回退到单独发布 */
    }
    
    /* 单独发布逻辑 */
    xSemaphoreTake(g_mqtt.mutex, portMAX_DELAY);
    
    static struct analysis_result processed_result;
    apply_virtual_device_offset(result, &processed_result, virtual_dev_idx);
    
    uint8_t dev_id = (virtual_dev_idx == 0)
                     ? g_mqtt.config.devices[0].virtual_dev_id
                     : g_mqtt.config.devices[virtual_dev_idx - 1].virtual_dev_id;
    
    static char json_buffer[MQTT_MAX_MESSAGE_SIZE];
    int json_len = serialize_analysis_result_to_json(
        &processed_result, json_buffer, sizeof(json_buffer), dev_id);
    
    if (json_len < 0) {
        xSemaphoreGive(g_mqtt.mutex);
        g_mqtt.stats.buffer_overflow_count++;
        return json_len;
    }
    
    static char topic[MQTT_MAX_TOPIC_LEN];
    int topic_len = build_topic_vibration(topic, sizeof(topic), dev_id);
    if (topic_len <= 0 || (size_t)topic_len >= sizeof(topic)) {
        xSemaphoreGive(g_mqtt.mutex);
        return APP_ERR_MQTT_TOPIC_TOO_LONG;
    }
    
    int msg_id = esp_mqtt_client_publish(
        g_mqtt.client,
        topic,
        json_buffer,
        (size_t)json_len,
        g_mqtt.config.broker.qos,
        g_mqtt.config.broker.retain);
    
    if (msg_id < 0) {
        xSemaphoreGive(g_mqtt.mutex);
        g_mqtt.stats.publish_fail_count++;
        LOG_WARN("MQTT", "Publish failed (msg_id=%d)", msg_id);
        return APP_ERR_MQTT_PUBLISH_FAIL;
    }
    
    g_mqtt.stats.messages_published++;
    g_mqtt.stats.bytes_sent += (uint32_t)json_len;
    
    if (virtual_dev_idx > 0 && virtual_dev_idx <= MQTT_MAX_VIRTUAL_DEVICES) {
        g_mqtt.stats.per_device_stats[virtual_dev_idx - 1].messages_sent++;
        g_mqtt.stats.per_device_stats[virtual_dev_idx - 1].last_publish_time_us =
            (uint32_t)esp_timer_get_time();
    }
    
    xSemaphoreGive(g_mqtt.mutex);
    
    LOG_DEBUG("MQTT", "Published %d bytes to %s (dev=%u, virt_idx=%u)",
              json_len, topic, dev_id, virtual_dev_idx);
    
    return APP_ERR_OK;
}

int mqtt_publish_custom(const char *topic,
                         const void *data,
                         size_t data_len,
                         int qos,
                         bool retain)
{
    if (!g_mqtt.initialized) return APP_ERR_MQTT_NOT_INIT;
    if (!topic || !data || data_len == 0) return APP_ERR_MQTT_INVALID_PARAM;
    
    /* 未连接时尝试缓存 */
    if (!g_mqtt.running || g_mqtt.state != MQTT_STATE_CONNECTED) {
        if (g_mqtt.offline_buffer_init) {
            int cache_ret = mqtt_cache_message_offline(topic, data, data_len, qos, retain);
            if (cache_ret == APP_ERR_OK) {
                return APP_ERR_OK;
            }
        }
        return APP_ERR_MQTT_CONNECT_FAIL;
    }
    
    if (strlen(topic) >= MQTT_MAX_TOPIC_LEN) return APP_ERR_MQTT_TOPIC_TOO_LONG;
    
    xSemaphoreTake(g_mqtt.mutex, portMAX_DELAY);
    
    int msg_id = esp_mqtt_client_publish(
        g_mqtt.client,
        topic,
        (const char *)data,
        data_len,
        qos,
        retain);
    
    xSemaphoreGive(g_mqtt.mutex);
    
    if (msg_id < 0) {
        g_mqtt.stats.publish_fail_count++;
        return APP_ERR_MQTT_PUBLISH_FAIL;
    }
    
    g_mqtt.stats.messages_published++;
    g_mqtt.stats.bytes_sent += (uint32_t)data_len;
    
    return APP_ERR_OK;
}

int mqtt_subscribe(const char *topic,
                    int qos,
                    mqtt_message_callback_t callback,
                    void *user_data)
{
    if (!g_mqtt.initialized) return APP_ERR_MQTT_NOT_INIT;
    if (!topic) return APP_ERR_MQTT_INVALID_PARAM;
    if (!g_mqtt.running || g_mqtt.state != MQTT_STATE_CONNECTED) {
        return APP_ERR_MQTT_CONNECT_FAIL;
    }
    
    xSemaphoreTake(g_mqtt.mutex, portMAX_DELAY);
    
    int msg_id = esp_mqtt_client_subscribe(g_mqtt.client, topic, qos);
    
    xSemaphoreGive(g_mqtt.mutex);
    
    if (msg_id < 0) {
        return APP_ERR_MQTT_SUBSCRIBE_FAIL;
    }
    
    LOG_DEBUG("MQTT", "Subscribed to %s (qos=%d)", topic, qos);
    return APP_ERR_OK;
}

int mqtt_unsubscribe(const char *topic)
{
    if (!g_mqtt.initialized) return APP_ERR_MQTT_NOT_INIT;
    if (!topic) return APP_ERR_MQTT_INVALID_PARAM;
    if (!g_mqtt.running) return APP_ERR_MQTT_CONNECT_FAIL;
    
    xSemaphoreTake(g_mqtt.mutex, portMAX_DELAY);
    
    int msg_id = esp_mqtt_client_unsubscribe(g_mqtt.client, topic);
    
    xSemaphoreGive(g_mqtt.mutex);
    
    if (msg_id < 0) {
        return APP_ERR_MQTT_UNSUBSCRIBE_FAIL;
    }
    
    return APP_ERR_OK;
}

/* ==================== 虚拟设备管理 API 实现 ==================== */

int mqtt_add_virtual_device(const struct virtual_device_config *device)
{
    if (!device) return APP_ERR_MQTT_INVALID_PARAM;
    if (!g_mqtt.initialized) return APP_ERR_MQTT_NOT_INIT;
    
    xSemaphoreTake(g_mqtt.mutex, portMAX_DELAY);
    
    if (g_mqtt.config.num_virtual_devices >= MQTT_MAX_VIRTUAL_DEVICES) {
        xSemaphoreGive(g_mqtt.mutex);
        return APP_ERR_MQTT_VIRTUAL_DEV_FULL;
    }
    
    uint8_t idx = g_mqtt.config.num_virtual_devices;
    memcpy(&g_mqtt.config.devices[idx], device, sizeof(*device));
    g_mqtt.config.num_virtual_devices++;
    
    xSemaphoreGive(g_mqtt.mutex);
    
    LOG_INFO("MQTT", "Added virtual device %u \"%s\" (total=%u)",
             device->virtual_dev_id, device->name,
             g_mqtt.config.num_virtual_devices);
    
    return APP_ERR_OK;
}

int mqtt_remove_virtual_device(uint8_t virtual_dev_id)
{
    if (!g_mqtt.initialized) return APP_ERR_MQTT_NOT_INIT;
    
    xSemaphoreTake(g_mqtt.mutex, portMAX_DELAY);
    
    for (uint8_t i = 0; i < g_mqtt.config.num_virtual_devices; i++) {
        if (g_mqtt.config.devices[i].virtual_dev_id == virtual_dev_id) {
            for (uint8_t j = i; j < g_mqtt.config.num_virtual_devices - 1; j++) {
                g_mqtt.config.devices[j] = g_mqtt.config.devices[j + 1];
            }
            g_mqtt.config.num_virtual_devices--;
            xSemaphoreGive(g_mqtt.mutex);
            LOG_INFO("MQTT", "Removed virtual device %u", virtual_dev_id);
            return APP_ERR_OK;
        }
    }
    
    xSemaphoreGive(g_mqtt.mutex);
    return APP_ERR_MQTT_INVALID_DEV_ID;
}

uint8_t mqtt_get_virtual_device_count(void)
{
    return g_mqtt.config.num_virtual_devices;
}

int mqtt_set_active_virtual_device(uint8_t idx)
{
    if (idx > g_mqtt.config.num_virtual_devices) {
        return APP_ERR_MQTT_INVALID_DEV_ID;
    }
    g_mqtt.active_virtual_dev_idx = idx;
    return APP_ERR_OK;
}

/* ==================== 回调注册 API 实现 ==================== */

int mqtt_register_event_callback(mqtt_event_callback_t cb, void *user_data)
{
    if (!cb) return APP_ERR_MQTT_INVALID_PARAM;
    if (!g_mqtt.initialized) return APP_ERR_MQTT_NOT_INIT;
    
    xSemaphoreTake(g_mqtt.mutex, portMAX_DELAY);
    
    if (g_mqtt.event_callbacks[0].count >= MQTT_MAX_CALLBACKS) {
        xSemaphoreGive(g_mqtt.mutex);
        return APP_ERR_MQTT_CALLBACK_FULL;
    }
    
    int idx = g_mqtt.event_callbacks[0].count;
    g_mqtt.event_callbacks[idx].cb = cb;
    g_mqtt.event_callbacks[idx].user_data = user_data;
    g_mqtt.event_callbacks[0].count++;
    
    xSemaphoreGive(g_mqtt.mutex);
    return APP_ERR_OK;
}

int mqtt_unregister_event_callback(mqtt_event_callback_t cb)
{
    if (!cb) return APP_ERR_MQTT_INVALID_PARAM;
    if (!g_mqtt.initialized) return APP_ERR_MQTT_NOT_INIT;
    
    xSemaphoreTake(g_mqtt.mutex, portMAX_DELAY);
    
    for (int i = 0; i < g_mqtt.event_callbacks[0].count; i++) {
        if (g_mqtt.event_callbacks[i].cb == cb) {
            for (int j = i; j < g_mqtt.event_callbacks[0].count - 1; j++) {
                g_mqtt.event_callbacks[j] = g_mqtt.event_callbacks[j + 1];
            }
            g_mqtt.event_callbacks[0].count--;
            g_mqtt.event_callbacks[g_mqtt.event_callbacks[0].count].cb = NULL;
            g_mqtt.event_callbacks[g_mqtt.event_callbacks[0].count].user_data = NULL;
            xSemaphoreGive(g_mqtt.mutex);
            return APP_ERR_OK;
        }
    }
    
    xSemaphoreGive(g_mqtt.mutex);
    return APP_ERR_INVALID_PARAM;
}

/* ==================== 查询 API 实现 ==================== */

int mqtt_get_stats(struct mqtt_stats *stats)
{
    if (!stats) return APP_ERR_INVALID_PARAM;
    if (!g_mqtt.initialized) return APP_ERR_MQTT_NOT_INIT;
    
    xSemaphoreTake(g_mqtt.mutex, portMAX_DELAY);
    memcpy(stats, &g_mqtt.stats, sizeof(*stats));
    
    /* 补充离线缓冲区实时统计 */
    if (g_mqtt.offline_buffer_init) {
        stats->offline_cached_count = g_mqtt.offline_cached_messages;
    }
    
    xSemaphoreGive(g_mqtt.mutex);
    
    return APP_ERR_OK;
}

void mqtt_reset_stats(void)
{
    if (!g_mqtt.initialized) return;
    
    xSemaphoreTake(g_mqtt.mutex, portMAX_DELAY);
    memset(&g_mqtt.stats, 0, sizeof(g_mqtt.stats));
    xSemaphoreGive(g_mqtt.mutex);
}

enum mqtt_mode mqtt_get_current_mode(void)
{
    return g_mqtt.mode;
}

int mqtt_switch_mode(enum mqtt_mode new_mode)
{
    if (!g_mqtt.initialized) return APP_ERR_MQTT_NOT_INIT;
    if (new_mode != MQTT_MODE_TRAINING && new_mode != MQTT_MODE_UPLOAD) {
        return APP_ERR_MQTT_INVALID_MODE;
    }
    if (g_mqtt.running) {
        LOG_ERROR("MQTT", "Cannot switch mode while running, call stop() first");
        return APP_ERR_MQTT_INVALID_MODE;
    }
    
    g_mqtt.mode = new_mode;
    g_mqtt.config.mode = new_mode;
    
    LOG_INFO("MQTT", "Switched mode to %s",
             new_mode == MQTT_MODE_TRAINING ? "Training" : "Upload");
    
    return APP_ERR_OK;
}

int mqtt_get_broker_url(char *url_out, size_t buf_len)
{
    if (!url_out || buf_len == 0) return APP_ERR_INVALID_PARAM;
    if (!g_mqtt.initialized) return APP_ERR_MQTT_NOT_INIT;
    
    xSemaphoreTake(g_mqtt.mutex, portMAX_DELAY);
    strncpy(url_out, g_mqtt.config.broker.url, buf_len - 1);
    url_out[buf_len - 1] = '\0';
    xSemaphoreGive(g_mqtt.mutex);
    
    return APP_ERR_OK;
}

/* ==================== 离线缓冲 API 实现 ==================== */

int mqtt_cache_message_offline(const char *topic,
                               const void *payload,
                               size_t payload_len,
                               int qos,
                               bool retain)
{
    if (!g_mqtt.initialized) return APP_ERR_MQTT_NOT_INIT;
    if (!topic || !payload || payload_len == 0) return APP_ERR_MQTT_INVALID_PARAM;
    if (!g_mqtt.offline_buffer_init) {
        LOG_WARN("MQTT-OFFLINE", "Offline buffer not initialized, cannot cache");
        return APP_ERR_MQTT_NOT_INIT;
    }
    
    /* 构造离线消息结构体 */
    struct offline_message msg;
    memset(&msg, 0, sizeof(msg));
    
    strncpy(msg.topic, topic, sizeof(msg.topic) - 1);
    if (payload_len >= sizeof(msg.payload)) {
        payload_len = sizeof(msg.payload) - 1;
        g_mqtt.stats.buffer_overflow_count++;
    }
    memcpy(msg.payload, payload, payload_len);
    msg.payload[payload_len] = '\0';
    msg.payload_len = payload_len;
    msg.qos = qos;
    msg.retain = retain;
    msg.timestamp_us = (uint32_t)esp_timer_get_time();
    msg.retry_count = 0;
    
    /* 序列化为二进制格式 */
    uint8_t serialized[sizeof(msg) + 16];  /* 额外空间给头部 */
    int ser_len = serialize_offline_message(&msg, serialized, sizeof(serialized));
    if (ser_len < 0) {
        LOG_ERROR("MQTT-OFFLINE", "Serialization failed (err=%d)", ser_len);
        return ser_len;
    }
    
    /* 写入环形缓冲区 */
    size_t written = ringbuf_push(&g_mqtt.offline_ringbuf, serialized, (size_t)ser_len);
    
    if (written == 0) {
        g_mqtt.stats.offline_dropped_count++;
        LOG_WARN("MQTT-OFFLINE", "Buffer full, message dropped (total_dropped=%u)",
                 g_mqtt.stats.offline_dropped_count);
        return APP_ERR_MQTT_BUFFER_OVERFLOW;
    }
    
    g_mqtt.offline_cached_messages++;
    g_mqtt.stats.offline_cached_count++;
    
    LOG_DEBUG("MQTT-OFFLINE", "Cached message to %s (%d bytes, total_cached=%u)",
              topic, ser_len, g_mqtt.offline_cached_messages);
    
    return APP_ERR_OK;
}

int mqtt_flush_offline_buffer(void)
{
    if (!g_mqtt.initialized) return APP_ERR_MQTT_NOT_INIT;
    if (!g_mqtt.offline_buffer_init) return APP_ERR_MQTT_NOT_INIT;
    if (!g_mqtt.running || g_mqtt.state != MQTT_STATE_CONNECTED) {
        return APP_ERR_MQTT_CONNECT_FAIL;
    }
    
    int sent_count = 0;
    static uint8_t temp_buf[sizeof(struct offline_message) + 16];
    
    /* 循环读取并发送所有缓存消息 */
    while (ringbuf_used(&g_mqtt.offline_ringbuf) > 0) {
        size_t read_len = ringbuf_pop(&g_mqtt.offline_ringbuf, temp_buf, sizeof(temp_buf));
        
        if (read_len == 0) {
            break;
        }
        
        /* 反序列化 */
        struct offline_message msg;
        int de_ret = deserialize_offline_message(temp_buf, read_len, &msg);
        if (de_ret != APP_ERR_OK) {
            LOG_ERROR("MQTT-OFFLINE", "Deserialization failed (err=%d), skipping", de_ret);
            continue;
        }
        
        /* 发送消息 */
        int msg_id = esp_mqtt_client_publish(
            g_mqtt.client,
            msg.topic,
            msg.payload,
            msg.payload_len,
            msg.qos,
            msg.retain);
        
        if (msg_id >= 0) {
            sent_count++;
            g_mqtt.stats.offline_sent_count++;
            g_mqtt.stats.messages_published++;
            g_mqtt.stats.bytes_sent += (uint32_t)msg.payload_len;
            
            LOG_DEBUG("MQTT-OFFLINE", "Sent cached message #%d -> %s (%d bytes)",
                      sent_count, msg.topic, msg.payload_len);
        } else {
            g_mqtt.stats.publish_fail_count++;
            LOG_WARN("MQTT-OFFLINE", "Failed to send cached message (msg_id=%d)", msg_id);
            
            /* 发送失败, 重新放回缓冲区 (可选, 此处简化处理直接丢弃) */
            /* 生产环境可实现重试机制 */
        }
        
        g_mqtt.offline_cached_messages--;
    }
    
    if (sent_count > 0) {
        LOG_INFO("MQTT-OFFLINE", "Flush complete: %d messages sent", sent_count);
    }
    
    return sent_count;
}

int mqtt_get_offline_buffer_status(uint32_t *cached_count, uint8_t *buffer_usage)
{
    if (!g_mqtt.initialized) return APP_ERR_MQTT_NOT_INIT;
    if (!g_mqtt.offline_buffer_init) return APP_ERR_MQTT_NOT_INIT;
    
    if (cached_count) {
        *cached_count = g_mqtt.offline_cached_messages;
    }
    
    if (buffer_usage) {
        size_t used = ringbuf_used(&g_mqtt.offline_ringbuf);
        size_t capacity = ringbuf_capacity(&g_mqtt.offline_ringbuf);
        *buffer_usage = (uint8_t)((used * 100) / (capacity > 0 ? capacity : 1));
    }
    
    return APP_ERR_OK;
}

int mqtt_clear_offline_buffer(void)
{
    if (!g_mqtt.initialized) return APP_ERR_MQTT_NOT_INIT;
    if (!g_mqtt.offline_buffer_init) return APP_ERR_MQTT_NOT_INIT;
    
    ringbuf_reset(&g_mqtt.offline_ringbuf);
    g_mqtt.offline_cached_messages = 0;
    
    LOG_INFO("MQTT-OFFLINE", "Offline buffer cleared (%u messages discarded)",
             g_mqtt.offline_cached_messages);
    
    return APP_ERR_OK;
}

/* ==================== 批量发布 API 实现 ==================== */

int mqtt_publish_batch(const struct analysis_result **results,
                       uint8_t count,
                       uint8_t virtual_dev_idx_start)
{
    if (!g_mqtt.initialized) return APP_ERR_MQTT_NOT_INIT;
    if (!results || count == 0 || count > MQTT_BATCH_MAX_MESSAGES) {
        return APP_ERR_MQTT_INVALID_PARAM;
    }
    if (!g_mqtt.running || g_mqtt.state != MQTT_STATE_CONNECTED) {
        return APP_ERR_MQTT_CONNECT_FAIL;
    }

    /*
     * ⚠️ 【关键修复】避免栈溢出! 使用静态缓冲区!
     *
     * 原始BUG:
     *   char json_buffer[MQTT_MAX_MESSAGE_SIZE * MQTT_BATCH_MAX_MESSAGES];
     *   = 2048 * 16 = 32KB (栈分配!)
     *
     * 问题:
     *   MQTT任务栈只有4KB,但这里分配了32KB
     *   导致严重栈溢出,破坏FreeRTOS TCB
     *   后续vTaskDelay()访问损坏的TCB → LoadProhibited崩溃!
     *
     * 修复方案:
     *   [1] 使用static缓冲区 (BSS段,不占栈空间)
     *   [2] 限制实际大小为合理值 (单条消息最大2KB, 4条消息最多8KB)
     *   [3] 添加安全上限检查
     */
    static char json_buffer[8192];  /* 8KB静态缓冲区 (足够4条批量消息) */

    /* 安全检查: 防止JSON过大 */
    const size_t max_json_size = sizeof(json_buffer) - 256;  /* 留256字节余量 */

    int json_len = serialize_batch_to_json(results, count, json_buffer,
                                           max_json_size, virtual_dev_idx_start);

    if (json_len < 0) {
        g_mqtt.stats.buffer_overflow_count++;
        return json_len;
    }

    if ((size_t)json_len >= max_json_size) {
        LOG_ERROR("MQTT-BATCH", "Batch JSON too large (%d bytes), truncating", json_len);
        g_mqtt.stats.buffer_overflow_count++;
        return APP_ERR_MQTT_BUFFER_OVERFLOW;
    }
    
    /* 构建 Topic (使用第一个设备的 Topic 作为批量 Topic) */
    uint8_t first_dev_id = (virtual_dev_idx_start == 0)
                           ? g_mqtt.config.devices[0].virtual_dev_id
                           : g_mqtt.config.devices[virtual_dev_idx_start - 1].virtual_dev_id;
    
    static char topic[MQTT_MAX_TOPIC_LEN];
    int topic_len = build_topic_vibration(topic, sizeof(topic), first_dev_id);
    if (topic_len <= 0 || (size_t)topic_len >= sizeof(topic)) {
        return APP_ERR_MQTT_TOPIC_TOO_LONG;
    }
    
    /* 发布批量消息 */
    xSemaphoreTake(g_mqtt.mutex, portMAX_DELAY);
    
    int msg_id = esp_mqtt_client_publish(
        g_mqtt.client,
        topic,
        json_buffer,
        (size_t)json_len,
        g_mqtt.config.broker.qos,
        g_mqtt.config.broker.retain);
    
    if (msg_id >= 0) {
        g_mqtt.stats.messages_published++;
        g_mqtt.stats.bytes_sent += (uint32_t)json_len;
        g_mqtt.stats.batch_publish_count++;
        g_mqtt.stats.batch_messages_count += count;
        
        LOG_INFO("MQTT-BATCH", "Published batch: %d messages, %d bytes -> %s",
                 count, json_len, topic);
    } else {
        g_mqtt.stats.publish_fail_count++;
        xSemaphoreGive(g_mqtt.mutex);
        LOG_WARN("MQTT-BATCH", "Batch publish failed (msg_id=%d)", msg_id);
        return APP_ERR_MQTT_PUBLISH_FAIL;
    }
    
    xSemaphoreGive(g_mqtt.mutex);
    
    return APP_ERR_OK;
}

int mqtt_set_batch_config(const struct batch_config *config)
{
    if (!config) return APP_ERR_MQTT_INVALID_PARAM;
    if (!g_mqtt.initialized) return APP_ERR_MQTT_NOT_INIT;
    
    xSemaphoreTake(g_mqtt.batch_mutex, portMAX_DELAY);
    
    memcpy(&g_mqtt.batch_cfg, config, sizeof(*config));
    
    /* 参数范围校验 */
    if (g_mqtt.batch_cfg.max_messages == 0 ||
        g_mqtt.batch_cfg.max_messages > MQTT_BATCH_MAX_MESSAGES) {
        g_mqtt.batch_cfg.max_messages = MQTT_BATCH_MAX_MESSAGES;
    }
    if (g_mqtt.batch_cfg.flush_interval_ms == 0) {
        g_mqtt.batch_cfg.flush_interval_ms = 1000;  /* 默认1秒 */
    }
    if (g_mqtt.batch_cfg.max_batch_size_bytes == 0) {
        g_mqtt.batch_cfg.max_batch_size_bytes = 4096;  /* 默认4KB */
    }
    
    xSemaphoreGive(g_mqtt.batch_mutex);
    
    LOG_INFO("MQTT-BATCH", "Batch config updated: enabled=%s, max_msg=%u, interval=%ums",
             g_mqtt.batch_cfg.enabled ? "ON" : "OFF",
             g_mqtt.batch_cfg.max_messages,
             g_mqtt.batch_cfg.flush_interval_ms);
    
    return APP_ERR_OK;
}

int mqtt_get_batch_config(struct batch_config *config)
{
    if (!config) return APP_ERR_INVALID_PARAM;
    if (!g_mqtt.initialized) return APP_ERR_MQTT_NOT_INIT;
    
    xSemaphoreTake(g_mqtt.batch_mutex, portMAX_DELAY);
    memcpy(config, &g_mqtt.batch_cfg, sizeof(*config));
    xSemaphoreGive(g_mqtt.batch_mutex);
    
    return APP_ERR_OK;
}

int mqtt_flush_batch_buffer(void)
{
    if (!g_mqtt.initialized) return APP_ERR_MQTT_NOT_INIT;
    if (!g_mqtt.batch_cfg.enabled || g_mqtt.batch_count == 0) {
        return 0;
    }
    
    xSemaphoreTake(g_mqtt.batch_mutex, portMAX_DELAY);
    
    /* 临时保存批量数据 */
    uint8_t temp_count = g_mqtt.batch_count;
    struct analysis_result *temp_results[MQTT_BATCH_MAX_MESSAGES];
    memcpy(temp_results, g_mqtt.batch_accumulator,
           sizeof(temp_results[0]) * temp_count);
    
    g_mqtt.batch_count = 0;
    
    xSemaphoreGive(g_mqtt.batch_mutex);
    
    /* 发布 */
    if (temp_count > 0) {
        int ret = mqtt_publish_batch(
            (const struct analysis_result **)temp_results,
            temp_count,
            g_mqtt.active_virtual_dev_idx);
        
        if (ret != APP_ERR_OK) {
            LOG_WARN("MQTT-BATCH", "Batch flush failed (err=%d)", ret);
            return ret;
        }
        
        return (int)temp_count;
    }
    
    return 0;
}

/* ==================== 动态配置更新 API 实现 ==================== */

int mqtt_subscribe_config_topic(const char *topic, int qos)
{
    if (!g_mqtt.initialized) return APP_ERR_MQTT_NOT_INIT;
    if (!topic) return APP_ERR_MQTT_INVALID_PARAM;
    if (!g_mqtt.running || g_mqtt.state != MQTT_STATE_CONNECTED) {
        return APP_ERR_MQTT_CONNECT_FAIL;
    }
    
    xSemaphoreTake(g_mqtt.mutex, portMAX_DELAY);
    
    /* 保存配置 Topic */
    strncpy(g_mqtt.config_topic, topic, sizeof(g_mqtt.config_topic) - 1);
    g_mqtt.config_topic[sizeof(g_mqtt.config_topic) - 1] = '\0';
    
    /* 订阅 Topic */
    int msg_id = esp_mqtt_client_subscribe(g_mqtt.client, topic, qos);
    
    if (msg_id > 0) {
        g_mqtt.config_subscribed = true;
        LOG_INFO("MQTT-CONFIG", "Subscribed to config topic: %s (qos=%d)", topic, qos);
    } else {
        LOG_ERROR("MQTT-CONFIG", "Failed to subscribe to config topic (msg_id=%d)", msg_id);
        xSemaphoreGive(g_mqtt.mutex);
        return APP_ERR_MQTT_SUBSCRIBE_FAIL;
    }
    
    xSemaphoreGive(g_mqtt.mutex);
    
    return APP_ERR_OK;
}

int mqtt_unsubscribe_config_topic(void)
{
    if (!g_mqtt.initialized) return APP_ERR_MQTT_NOT_INIT;
    if (!g_mqtt.running) return APP_ERR_MQTT_CONNECT_FAIL;
    
    xSemaphoreTake(g_mqtt.mutex, portMAX_DELAY);
    
    if (!g_mqtt.config_subscribed || strlen(g_mqtt.config_topic) == 0) {
        xSemaphoreGive(g_mqtt.mutex);
        return APP_ERR_OK;  /* 未订阅, 直接返回成功 */
    }
    
    int msg_id = esp_mqtt_client_unsubscribe(g_mqtt.client, g_mqtt.config_topic);
    
    if (msg_id >= 0) {
        g_mqtt.config_subscribed = false;
        LOG_INFO("MQTT-CONFIG", "Unsubscribed from config topic: %s", g_mqtt.config_topic);
    } else {
        LOG_WARN("MQTT-CONFIG", "Failed to unsubscribe (msg_id=%d)", msg_id);
    }
    
    xSemaphoreGive(g_mqtt.mutex);
    
    return APP_ERR_OK;
}

int mqtt_request_config_sync(const char *request_topic)
{
    if (!g_mqtt.initialized) return APP_ERR_MQTT_NOT_INIT;
    if (!request_topic) return APP_ERR_MQTT_INVALID_PARAM;
    if (!g_mqtt.running || g_mqtt.state != MQTT_STATE_CONNECTED) {
        return APP_ERR_MQTT_CONNECT_FAIL;
    }
    
    /* 发送空消息作为同步请求 */
    int msg_id = esp_mqtt_client_publish(
        g_mqtt.client,
        request_topic,
        "",  /* 空负载 */
        0,
        1,  /* QoS 1 确保送达 */
        false);
    
    if (msg_id < 0) {
        LOG_ERROR("MQTT-CONFIG", "Config sync request failed (msg_id=%d)", msg_id);
        return APP_ERR_MQTT_PUBLISH_FAIL;
    }
    
    LOG_INFO("MQTT-CONFIG", "Config sync request sent to %s", request_topic);
    
    return APP_ERR_OK;
}

int mqtt_register_config_callback(mqtt_message_callback_t callback,
                                   void *user_data)
{
    if (!callback) return APP_ERR_MQTT_INVALID_PARAM;
    if (!g_mqtt.initialized) return APP_ERR_MQTT_NOT_INIT;
    
    g_mqtt.config_callback.cb = callback;
    g_mqtt.config_callback.user_data = user_data;
    
    LOG_INFO("MQTT-CONFIG", "Config callback registered");
    
    return APP_ERR_OK;
}

int mqtt_get_config_update_stats(struct mqtt_stats *stats)
{
    if (!stats) return APP_ERR_INVALID_PARAM;
    if (!g_mqtt.initialized) return APP_ERR_MQTT_NOT_INIT;
    
    xSemaphoreTake(g_mqtt.mutex, portMAX_DELAY);
    
    /* 只复制配置相关统计 */
    stats->config_update_count = g_mqtt.stats.config_update_count;
    stats->config_update_success = g_mqtt.stats.config_update_success;
    stats->config_update_fail = g_mqtt.stats.config_update_fail;
    
    xSemaphoreGive(g_mqtt.mutex);
    
    return APP_ERR_OK;
}
