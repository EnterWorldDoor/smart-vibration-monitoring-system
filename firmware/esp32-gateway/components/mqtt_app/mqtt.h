/**
 * @file mqtt.h
 * @author EnterWorldDoor
 * @brief 企业级 MQTT 通信模块 (双模式 + 虚拟设备模拟)
 *
 * 功能特性:
 *   - 双模式运行:
 *     [1] 训练模式 (Training): 发送 RMS/FFT 数据到 PC 端 (本地 Broker)
 *     [2] 上传模式 (Upload):   发送数据到树莓派网关
 *   - 虚拟设备模拟: 单个 ESP32 物理设备模拟 N 台虚拟设备
 *   - 基于 ESP-IDF esp-mqtt 组件 (MQTT 3.1.1)
 *   - JSON 消息格式 (标准化输出供 AI/MQTT 使用)
 *   - TLS 加密传输 (可选)
 *   - 自动重连机制 + QoS 保证
 *   - FreeRTOS Mutex 线程安全
 *   - 统计监控: TX/RX/Error 计数器
 *
 * 适用场景:
 *   - 工业振动监测系统数据上报
 *   - EdgeVib 设备群组管理
 *   - AI 训练数据采集 (PC 端)
 *   - 云端/边缘计算节点通信 (树莓派)
 *
 * Topic 设计:
 *   ┌─────────────────────────────────────────────┐
 *   │ Training Mode (PC 端)                       │
 *   │ edgevib/train/{dev_id}/vibration  ← RMS/FFT │
 *   │ edgevib/train/{dev_id}/environment ← 温湿度 │
 *   ├─────────────────────────────────────────────┤
 *   │ Upload Mode (树莓派)                        │
 *   │ edgevib/upload/{dev_id}/vibration          │
 *   │ edgevib/upload/{dev_id}/status             │
 *   │ edgevib/upload/{dev_id}/health             │
 *   └─────────────────────────────────────────────┘
 *
 * 虚拟设备模拟原理:
 *   - 配置 N 个虚拟设备 ID 和参数偏移
 *   - 实际传感器数据 + 偏移量 = 虚拟设备数据
 *   - 轮流发送或同时发送 (可配置)
 *   - 每个 virtual device 有独立统计信息
 */

#ifndef MQTT_H
#define MQTT_H

#include <stdint.h>
#include <stdbool.h>
#include "global_error.h"
#include "sensor_service.h"

/* ==================== 配置常量 ==================== */

#define MQTT_MAX_TOPIC_LEN            128      /**< 最大 Topic 长度 */
#define MQTT_MAX_MESSAGE_SIZE         2048     /**< 最大消息体大小 (字节) */
#define MQTT_MAX_VIRTUAL_DEVICES      8        /**< 最大虚拟设备数量 */
#define MQTT_MAX_CLIENT_ID_LEN        64       /**< 最大 Client ID 长度 */
#define MQTT_MAX_BROKER_URL_LEN       256      /**< 最大 Broker URL 长度 */
#define MQTT_TASK_STACK_SIZE          4096     /**< MQTT 后台任务栈大小 (仅批量检查,不需要大栈) */
#define MQTT_TASK_PRIORITY            5        /**< MQTT 任务优先级 */
#define MQTT_CONNECT_TIMEOUT_MS       10000    /**< 连接超时 (ms) */
#define MQTT_RECONNECT_INTERVAL_MS    5000     /**< 重连间隔 (ms) */
#define MQTT_KEEPALIVE_SECONDS        120      /**< Keep-Alive 时间 (秒) */
#define MQTT_QOS_DEFAULT              1        /**< 默认 QoS 等级 */
#define MQTT_MAX_CALLBACKS            4        /**< 最大事件回调数 */
#define MQTT_TLS_CERT_MAX_LEN         4096     /**< TLS 证书最大长度 (字节) */
#define MQTT_OFFLINE_BUFFER_SIZE      8192     /**< 离线缓冲区大小 (字节) */
#define MQTT_BATCH_MAX_MESSAGES       4        /**< 批量发布最大消息数 */
#define MQTT_CONFIG_TOPIC_LEN         128      /**< 配置订阅 Topic 长度 */

/* ==================== 运行模式枚举 ==================== */

enum mqtt_mode {
    MQTT_MODE_TRAINING = 0,         /**< 训练模式: 发送数据到 PC 端本地 Broker */
    MQTT_MODE_UPLOAD = 1            /**< 上传模式: 发送数据到树莓派网关 */
};

/* ==================== 连接状态枚举 ==================== */

enum mqtt_connection_state {
    MQTT_STATE_DISCONNECTED = 0,    /**< 未连接 */
    MQTT_STATE_CONNECTING,          /**< 正在连接 */
    MQTT_STATE_CONNECTED,           /**< 已连接 */
    MQTT_STATE_RECONNECTING,        /**< 重连中 */
    MQTT_STATE_ERROR                /**< 错误状态 */
};

/* ==================== 数据结构 ==================== */

/**
 * struct mqtt_broker_config - MQTT Broker 连接配置
 */
struct mqtt_broker_config {
    char url[MQTT_MAX_BROKER_URL_LEN];  /**< Broker URL (mqtt://host:port 或 mqtts://host:port) */
    uint16_t port;                      /**< 端口号 (覆盖 URL 中的端口, 0=使用URL中的) */
    char username[64];                  /**< 用户名 (可选) */
    char password[64];                  /**< 密码 (可选) */
    char client_id[MQTT_MAX_CLIENT_ID_LEN]; /**< 客户端 ID */
    bool enable_tls;                   /**< 是否启用 TLS */
    bool clean_session;                /**< 是否清除会话 */
    uint16_t keepalive_sec;            /**< Keep-Alive 时间 (秒) */
    uint8_t qos;                       /**< 默认 QoS (0/1/2) */
    bool retain;                       /**< 是否保留消息 */
    
    /* TLS 配置扩展 */
    char ca_cert[MQTT_TLS_CERT_MAX_LEN]; /**< CA 证书 PEM 格式 (enable_tls=true时必填) */
    char client_cert[MQTT_TLS_CERT_MAX_LEN]; /**< 客户端证书 PEM 格式 (双向认证) */
    char client_key[MQTT_TLS_CERT_MAX_LEN];  /**< 客户端私钥 PEM 格式 (双向认证) */
    bool use_global_ca_store;          /**< 使用 ESP-IDF 全局 CA 存储 */
    bool skip_cert_common_name_check;  /**< 跳过 CN 验证 (测试用, 生产环境禁用) */
};

/**
 * struct virtual_device_config - 虚拟设备配置
 *
 * 用于单 ESP32 模拟多台设备场景:
 *   - 每个虚拟设备有独立的 dev_id 和参数偏移
 *   - 实际数据经过偏移处理后作为虚拟设备数据发送
 */
struct virtual_device_config {
    uint8_t virtual_dev_id;            /**< 虚拟设备 ID (1~255) */
    char name[32];                    /**< 设备名称 (如 "Motor_01") */
    float vibration_offset_x;         /**< X轴振动偏移 (g), 用于模拟不同位置 */
    float vibration_offset_y;         /**< Y轴振动偏移 (g) */
    float vibration_offset_z;         /**< Z轴振动偏移 (g) */
    float temp_offset_c;              /**< 温度偏移 (°C) */
    float humidity_offset_rh;         /**< 湿度偏移 (%RH) */
    bool enabled;                     /**< 是否启用此虚拟设备 */
};

/**
 * struct mqtt_config - MQTT 模块完整配置
 */
struct mqtt_config {
    enum mqtt_mode mode;               /**< 运行模式 (Training/Upload) */
    struct mqtt_broker_config broker;  /**< Broker 连接配置 */
    
    /* 虚拟设备配置 */
    uint8_t num_virtual_devices;       /**< 虚拟设备数量 (1=不模拟, 2~MAX) */
    struct virtual_device_config devices[MQTT_MAX_VIRTUAL_DEVICES];
    
    /* 发布控制 */
    uint32_t publish_interval_ms;      /**< 发布间隔 (ms), 默认 1000 */
    bool publish_vibration_data;       /**< 是否发布振动数据 */
    bool publish_environment_data;     /**< 是否发布环境数据 (温湿度) */
    bool publish_health_status;        /**< 是否发布健康状态 */
    
    /* LWT (Last Will and Testament) */
    bool enable_lwt;                   /**< 是否启用 LWT */
    char lwt_topic[MQTT_MAX_TOPIC_LEN];/**< LWT Topic */
    char lwt_message[128];             /**< LWT 消息内容 */
};

/**
 * struct mqtt_stats - MQTT 统计信息
 */
struct mqtt_stats {
    /* 连接统计 */
    uint32_t connect_count;            /**< 累计连接次数 */
    uint32_t disconnect_count;         /**< 累计断开次数 */
    uint32_t reconnect_count;          /**< 累计重连次数 */
    int64_t last_connect_time_us;      /**< 上次连接时间戳 */
    int64_t uptime_seconds;            /**< 当前连接持续时间 (秒) */
    
    /* 发布统计 */
    uint32_t messages_published;       /**< 累计发布消息数 */
    uint32_t bytes_sent;               /**< 累计发送字节数 */
    uint32_t publish_success_count;    /**< 发布成功次数 */
    uint32_t publish_fail_count;       /**< 发布失败次数 */
    
    /* 错误统计 */
    uint32_t connection_errors;        /**< 连接错误次数 */
    uint32_t timeout_errors;           /**< 超时错误次数 */
    uint32_t buffer_overflow_count;    /**< 缓冲区溢出次数 */
    
    /* 离线缓冲统计 */
    uint32_t offline_cached_count;     /**< 离线缓存消息数 */
    uint32_t offline_sent_count;       /**< 恢复后补发消息数 */
    uint32_t offline_dropped_count;    /**< 缓冲区满丢弃消息数 */
    
    /* 批量发布统计 */
    uint32_t batch_publish_count;      /**< 批量发布次数 */
    uint32_t batch_messages_count;     /**< 批量发布消息总数 */
    
    /* 配置更新统计 */
    uint32_t config_update_count;      /**< 远程配置更新次数 */
    uint32_t config_update_success;    /**< 配置更新成功次数 */
    uint32_t config_update_fail;       /**< 配置更新失败次数 */
    
    /* 虚拟设备统计 (按设备索引) */
    struct {
        uint32_t messages_sent;        /**< 该虚拟设备发送的消息数 */
        uint32_t last_publish_time_us; /**< 上次发布时间 */
    } per_device_stats[MQTT_MAX_VIRTUAL_DEVICES];
};

/**
 * struct offline_message - 离线缓存消息格式
 *
 * 用于在网络断开时缓存待发送消息:
 *   - 包含完整的 Topic 和 Payload
 *   - 恢复连接后按顺序补发
 *   - 支持优先级和 QoS 保留
 */
struct offline_message {
    char topic[MQTT_MAX_TOPIC_LEN];    /**< 目标 Topic */
    char payload[MQTT_MAX_MESSAGE_SIZE]; /**< 消息内容 */
    size_t payload_len;                /**< 消息长度 */
    int qos;                           /**< QoS 等级 */
    bool retain;                       /**< 是否保留 */
    uint32_t timestamp_us;             /**< 入队时间戳 (用于超时清理) */
    uint8_t retry_count;               /**< 重试次数 */
};

/**
 * struct batch_config - 批量发布配置
 */
struct batch_config {
    bool enabled;                      /**< 是否启用批量发布 */
    uint8_t max_messages;              /**< 单次批量最大消息数 (1~BATCH_MAX) */
    uint32_t flush_interval_ms;        /**< 刷新间隔 (ms), 超时自动发送 */
    uint16_t max_batch_size_bytes;     /**< 批量数据最大字节限制 */
    bool merge_same_topic;             /**< 合并相同 Topic 的消息 */
};

/* ==================== 回调函数类型 ==================== */

/**
 * mqtt_event_callback_t - MQTT 事件回调
 * @event_type: 事件类型 (连接/断开/发布成功/错误等)
 * @context: 事件描述字符串
 * @user_data: 用户上下文
 */
typedef void (*mqtt_event_callback_t)(int event_type,
                                      const char *context,
                                      void *user_data);

/**
 * mqtt_message_callback_t - 收到订阅消息的回调
 * @topic: 来源 Topic
 * @data: 消息数据指针
 * @data_len: 数据长度
 * @user_data: 用户上下文
 */
typedef void (*mqtt_message_callback_t)(const char *topic,
                                        const void *data,
                                        size_t data_len,
                                        void *user_data);

/* ==================== 生命周期 API ==================== */

/**
 * mqtt_init - 初始化 MQTT 模块
 * @config: 配置参数 (NULL 使用默认配置)
 *
 * 内部完成:
 *   - 参数验证和默认值填充
 *   - 互斥量创建
 *   - 统计计数器初始化
 *   - 不立即连接 Broker (需调用 mqtt_start)
 *
 * Return: APP_ERR_OK or error code
 */
int mqtt_init(const struct mqtt_config *config);

/**
 * mqtt_deinit - 反初始化并释放资源
 *
 * 内部完成:
 *   - 断开连接
 *   - 释放资源
 *   - 清理任务
 *
 * Return: APP_ERR_OK or error code
 */
int mqtt_deinit(void);

/**
 * mqtt_is_initialized - 查询是否已初始化
 */
bool mqtt_is_initialized(void);

/* ==================== 连接管理 API ==================== */

/**
 * mqtt_start - 启动 MQTT 客户端 (连接 Broker)
 *
 * 创建后台 FreeRTOS 任务处理:
 *   - TCP/TLS 连接建立
 *   - MQTT 握手协议
 *   - 心跳保活
 *   - 自动重连
 *
 * Return: APP_ERR_OK or error code
 */
int mqtt_start(void);

/**
 * mqtt_stop - 停止 MQTT 客户端 (断开连接)
 */
int mqtt_stop(void);

/**
 * mqtt_reconnect - 手动触发重新连接
 */
int mqtt_reconnect(void);

/**
 * mqtt_get_state - 获取当前连接状态
 */
enum mqtt_connection_state mqtt_get_state(void);

/**
 * mqtt_is_connected - 查询是否已连接
 */
bool mqtt_is_connected(void);

/* ==================== 数据发布 API ==================== */

/**
 * mqtt_publish_analysis_result - 发布分析结果 (RMS/FFT 数据)
 * @result: 来自 sensor_service 的完整分析结果
 * @virtual_dev_idx: 虚拟设备索引 (0=物理设备本身, 1~N=虚拟设备)
 *
 * 此函数是 MQTT 模块的核心接口:
 *   - 将 analysis_result 序列化为 JSON
 *   - 根据 mode 选择正确的 Topic 前缀
 *   - 应用虚拟设备偏移量
 *   - 异步发送 (不阻塞调用者)
 *
 * Return: APP_ERR_OK or error code
 */
int mqtt_publish_analysis_result(const struct analysis_result *result,
                                 uint8_t virtual_dev_idx);

/**
 * mqtt_publish_custom - 发布自定义消息
 * @topic: 目标 Topic
 * @data: 消息数据
 * @data_len: 数据长度
 * @qos: QoS 等级 (0/1/2)
 * @retain: 是否保留消息
 *
 * 用于:
 *   - AI 推理结果上报
 *   - 控制命令响应
 *   - 自定义业务数据
 *
 * Return: APP_ERR_OK or error code
 */
int mqtt_publish_custom(const char *topic,
                        const void *data,
                        size_t data_len,
                        int qos,
                        bool retain);

/**
 * mqtt_subscribe - 订阅 Topic
 * @topic: 要订阅的 Topic (支持通配符 +/#)
 * @qos: QoS 等级
 * @callback: 消息接收回调
 * @user_data: 用户上下文
 *
 * Return: APP_ERR_OK or error code
 */
int mqtt_subscribe(const char *topic,
                   int qos,
                   mqtt_message_callback_t callback,
                   void *user_data);

/**
 * mqtt_unsubscribe - 取消订阅 Topic
 * @topic: 要取消订阅的 Topic
 */
int mqtt_unsubscribe(const char *topic);

/* ==================== 虚拟设备管理 API ==================== */

/**
 * mqtt_add_virtual_device - 动态添加虚拟设备
 * @device: 虚拟设备配置
 *
 * Return: APP_ERR_OK or error code (APP_ERR_MQTT_VIRTUAL_DEV_FULL if full)
 */
int mqtt_add_virtual_device(const struct virtual_device_config *device);

/**
 * mqtt_remove_virtual_device - 移除虚拟设备
 * @virtual_dev_id: 虚拟设备 ID
 */
int mqtt_remove_virtual_device(uint8_t virtual_dev_id);

/**
 * mqtt_get_virtual_device_count - 获取当前虚拟设备数量
 */
uint8_t mqtt_get_virtual_device_count(void);

/**
 * mqtt_set_active_virtual_device - 设置当前活跃的虚拟设备 (用于轮流模式)
 * @idx: 虚拟设备索引
 */
int mqtt_set_active_virtual_device(uint8_t idx);

/* ==================== 回调注册 API ==================== */

/**
 * mqtt_register_event_callback - 注册事件回调
 * @cb: 回调函数
 * @user_data: 用户上下文
 */
int mqtt_register_event_callback(mqtt_event_callback_t cb,
                                  void *user_data);

/**
 * mqtt_unregister_event_callback - 注销事件回调
 * @cb: 待注销的回调函数
 */
int mqtt_unregister_event_callback(mqtt_event_callback_t cb);

/* ==================== 查询 API ==================== */

/**
 * mqtt_get_stats - 获取统计信息
 * @stats: 输出统计结构体
 */
int mqtt_get_stats(struct mqtt_stats *stats);

/**
 * mqtt_reset_stats - 重置统计计数器
 */
void mqtt_reset_stats(void);

/**
 * mqtt_get_current_mode - 获取当前运行模式
 */
enum mqtt_mode mqtt_get_current_mode(void);

/**
 * mqtt_switch_mode - 切换运行模式 (需先停止)
 * @new_mode: 新模式
 *
 * 注意: 切换模式前必须先调用 mqtt_stop()
 */
int mqtt_switch_mode(enum mqtt_mode new_mode);

/**
 * mqtt_get_broker_url - 获取当前 Broker URL
 * @url_out: 输出缓冲区
 * @buf_len: 缓冲区长度
 */
int mqtt_get_broker_url(char *url_out, size_t buf_len);

/* ==================== 离线缓冲 API ==================== */

/**
 * mqtt_cache_message_offline - 离线缓存消息 (网络断开时调用)
 * @topic: 目标 Topic
 * @payload: 消息内容
 * @payload_len: 消息长度
 * @qos: QoS 等级
 * @retain: 是否保留消息 *
 *
 * 功能:
 *   - 将消息写入 RingBuffer 缓存
 *   - 连接恢复后自动补发 (在 mqtt_task_main 中触发)
 *   - 缓冲区满时根据配置决定覆盖或丢弃
 *
 * Return: APP_ERR_OK or error code
 */
int mqtt_cache_message_offline(const char *topic,
                               const void *payload,
                               size_t payload_len,
                               int qos,
                               bool retain);

/**
 * mqtt_flush_offline_buffer - 手动刷新离线缓冲区 (立即发送所有缓存消息)
 *
 * 通常在连接恢复时自动调用，也可手动触发
 *
 * Return: 实际发送的消息数, 负数表示错误
 */
int mqtt_flush_offline_buffer(void);

/**
 * mqtt_get_offline_buffer_status - 获取离线缓冲区状态
 * @cached_count: 输出当前缓存消息数 (可为 NULL)
 * @buffer_usage: 输出缓冲区使用率 0~100 (可为 NULL)
 *
 * Return: APP_ERR_OK or error code
 */
int mqtt_get_offline_buffer_status(uint32_t *cached_count,
                                   uint8_t *buffer_usage);

/**
 * mqtt_clear_offline_buffer - 清空离线缓冲区 (丢弃所有缓存消息)
 *
 * Return: APP_ERR_OK or error code
 */
int mqtt_clear_offline_buffer(void);

/* ==================== 批量发布 API ==================== */

/**
 * mqtt_publish_batch - 批量发布多个虚拟设备的数据
 * @results: 分析结果数组
 * @count: 结果数量 (1 ~ MQTT_BATCH_MAX_MESSAGES)
 * @virtual_dev_idx_start: 起始虚拟设备索引
 *
 * 功能:
 *   - 将多个虚拟设备的分析结果合并为 JSON 数组
 *   - 单次发布减少网络开销
 *   - 自动处理 Topic 路由和偏移量应用
 *
 * JSON 格式:
 * {
 *   "batch": true,
 *   "count": N,
 *   "messages": [ {...}, {...}, ... ]
 * }
 *
 * Return: APP_ERR_OK or error code
 */
int mqtt_publish_batch(const struct analysis_result **results,
                       uint8_t count,
                       uint8_t virtual_dev_idx_start);

/**
 * mqtt_set_batch_config - 配置批量发布参数
 * @config: 批量发布配置
 *
 * Return: APP_ERR_OK or error code
 */
int mqtt_set_batch_config(const struct batch_config *config);

/**
 * mqtt_get_batch_config - 获取当前批量发布配置
 * @config: 输出配置结构体
 *
 * Return: APP_ERR_OK or error code
 */
int mqtt_get_batch_config(struct batch_config *config);

/**
 * mqtt_flush_batch_buffer - 刷新批量缓冲区 (立即发送累积的消息)
 *
 * Return: 发送的消息数, 负数表示错误
 */
int mqtt_flush_batch_buffer(void);

/* ==================== 动态配置更新 API ==================== */

/**
 * mqtt_subscribe_config_topic - 订阅远程配置更新 Topic
 * @topic: 配置 Topic (如 "edgevib/config/{device_id}/update")
 * @qos: QoS 等级
 *
 * 功能:
 *   - 订阅指定 Topic 监听远程配置推送
 *   - 自动解析 JSON 配置消息
 *   - 验证后通过 config_manager 更新全局配置
 *   - 触发配置变更回调通知其他模块
 *
 * 支持的 JSON 格式:
 * {
 *   "version": 1,
 *   "timestamp_ms": 1699876543210,
 *   "updates": {
 *     "mqtt_mode": 1,
 *     "mqtt_publish_interval_ms": 2000,
 *     ...
 *   }
 * }
 *
 * Return: APP_ERR_OK or error code
 */
int mqtt_subscribe_config_topic(const char *topic, int qos);

/**
 * mqtt_unsubscribe_config_topic - 取消订阅配置更新 Topic
 *
 * Return: APP_ERR_OK or error code
 */
int mqtt_unsubscribe_config_topic(void);

/**
 * mqtt_request_config_sync - 请求配置同步 (向服务器请求最新配置)
 * @request_topic: 配置请求 Topic
 *
 * 发送空消息或同步请求到指定 Topic
 *
 * Return: APP_ERR_OK or error code
 */
int mqtt_request_config_sync(const char *request_topic);

/**
 * mqtt_register_config_callback - 注册配置更新回调
 * @callback: 回调函数 (收到新配置时调用)
 * @user_data: 用户上下文
 *
 * Return: APP_ERR_OK or error code
 */
int mqtt_register_config_callback(mqtt_message_callback_t callback,
                                  void *user_data);

/**
 * mqtt_get_config_update_stats - 获取配置更新统计
 * @stats: 输出统计信息
 *
 * Return: APP_ERR_OK or error code
 */
int mqtt_get_config_update_stats(struct mqtt_stats *stats);

#endif /* MQTT_H */
