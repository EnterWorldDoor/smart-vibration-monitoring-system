/**
 * @file config_manager.h
 * @author EnterWorldDoor
 * @brief 企业级配置管理器：基于 ESP32 NVS，支持线程安全、配置验证、版本迁移、
 *        CRC 完整性校验、工厂重置、导出导入及备份恢复
 */

#ifndef CONFIG_MANAGER_H_
#define CONFIG_MANAGER_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "global_error.h"
#include "log_system.h"

/* 配置版本号 */
#define CONFIG_VERSION_MAJOR 1
#define CONFIG_VERSION_MINOR 0
#define CONFIG_VERSION_PATCH 0
#define CONFIG_VERSION ((CONFIG_VERSION_MAJOR << 16) | (CONFIG_VERSION_MINOR << 8) | CONFIG_VERSION_PATCH)

/* 配置项最大长度 */
#define MAX_CONFIG_KEY_LEN       32
#define MAX_CONFIG_VALUE_LEN     256
#define CONFIG_EXPORT_BUF_SIZE   (sizeof(struct system_config) + sizeof(uint32_t))  /* 配置体 + CRC32 */
#define CONFIG_MAX_BACKUP_SLOTS  10

/* 系统全局配置结构 */
struct system_config {
    /* ===== 元数据 ===== */
    uint32_t config_version;          /**< 配置结构体版本号 (CONFIG_VERSION)，用于迁移判断 */
    uint32_t crc32;                  /**< CRC32 校验和，存储时计算，读取时验证 */
    
    /* ===== ADXL345 加速度计配置 ===== */
    int adxl345_spi_host_id;         /**< SPI 主机 ID (SPI2_HOST=1, SPI3_HOST=2), 默认 SPI2_HOST */
    int adxl345_gpio_cs;             /**< CS 引脚号, 默认 5 */
    int adxl345_gpio_miso;           /**< MISO 引脚号, 默认 19 */
    int adxl345_gpio_mosi;           /**< MOSI 引脚号, 默认 23 */
    int adxl345_gpio_sclk;           /**< SCLK 引脚号, 默认 18 */
    int adxl345_clock_speed_hz;      /**< SPI 时钟频率 (Hz), 默认 5000000 (5MHz) */
    int adxl345_range;               /**< 量程: 2/4/8/16G, 默认 16G */
    int adxl345_data_rate;           /**< 数据速率 (Hz): 100/200/400/800...3200, 默认 400 */
    int adxl345_fifo_mode;           /**< FIFO 模式: 0=Bypass, 1=FIFO, 2=Stream, 默认 Stream */
    int adxl345_ma_window_size;      /**< 移动平均窗口大小, 默认 16 */

    /* ===== DSP 数字信号处理配置 ===== */
    int fft_size;                    /**< FFT 点数 (必须为2的幂), 默认 512 */
    int dsp_window_type;            /**< 窗函数类型: 0=Rect, 1=Hann, 2=Hamming, 3=Blackman, 默认 Hann(1) */
    bool dsp_enable_dc_removal;     /**< 是否去除直流分量, 默认 true */
    float rms_warning_threshold;     /**< RMS 警告阈值 (g), 默认 2.0 */
    float rms_critical_threshold;   /**< RMS 严重阈值 (g), 默认 4.0 */
    float freq_peak_threshold;      /**< 频谱峰值异常阈值, 默认 0.5 */

    /* ===== Temperature Compensation 温度补偿配置 ===== */
    bool temp_comp_enabled;         /**< 是否启用温度补偿, 默认 true */
    float temp_comp_ewma_alpha;     /**< EWMA 滤波系数 (0~1), 默认 0.1 */
    float temp_comp_change_threshold; /**< 温度变化触发补偿阈值 (°C), 默认 0.5 */
    float temp_comp_rate_threshold;  /**< 温度变化率阈值 (°C/s), 默认 1.0 */
    int temp_comp_stale_data_ms;     /**< 数据陈旧判定时间 (ms), 默认 5000 */

    /* ===== Sensor Service 传感器服务配置 ===== */
    int sample_rate_hz;              /**< 采样率 (Hz), 默认 400 */
    int sensor_buffer_size;          /**< 环形缓冲区大小 (样本数), 默认 1024 */
    uint32_t analysis_interval_ms;   /**< 分析周期 (ms), 默认 1000 */
    bool sensor_enable_temp_from_protocol; /**< 是否从协议接收温度, 默认 true */
    bool sensor_enable_detailed_log; /**< 是否启用详细日志, 默认 false */



    /* ===== Protocol UART 通信协议配置 ===== */
    int protocol_uart_num;           /**< UART 端口号, 默认 UART_NUM_4 */
    int protocol_baud_rate;          /**< 波特率, 默认 115200 */
    int protocol_tx_pin;             /**< TX 引脚号, 默认 17 */
    int protocol_rx_pin;             /**< RX 引脚号, 默认 16 */
    bool protocol_enable_ack;        /**< 是否启用 ACK 机制, 默认 true */
    bool protocol_enable_heartbeat;  /**< 是否启用心跳, 默认 true */
    uint32_t protocol_ack_timeout_ms; /**< ACK 超时 (ms), 默认 500 */
    uint32_t protocol_heartbeat_interval_ms; /**< 心跳间隔 (ms), 默认 1000 */
    int protocol_max_retries;        /**< 最大重试次数, 默认 3 */
    bool protocol_debug_dump;         /**< 是否输出帧调试信息, 默认 false */

    /* ===== MQTT 通信配置 ===== */
    int mqtt_mode;                   /**< 运行模式: 0=Training(PC), 1=Upload(树莓派), 默认 Training(0) */
    char mqtt_broker_url[128];       /**< Broker URL, 默认 "mqtt://192.168.1.100:1883" (PC) */
    uint16_t mqtt_broker_port;        /**< Broker 端口, 默认 1883 */
    char mqtt_username[64];          /**< 用户名 (可选), 默认空 */
    char mqtt_password[64];          /**< 密码 (可选), 默认空 */
    char mqtt_client_id[64];         /**< 客户端 ID, 自动生成 */
    uint8_t mqtt_qos;                /**< QoS 等级 (0/1/2), 默认 1 */
    bool mqtt_enable_tls;            /**< 是否启用 TLS, 默认 false */
    bool mqtt_clean_session;         /**< 清除会话, 默认 true */
    uint32_t mqtt_keepalive_sec;     /**< Keep-Alive 时间 (秒), 默认 120 */
    uint32_t mqtt_publish_interval_ms; /**< 发布间隔 (ms), 默认 1000 */
    uint8_t mqtt_num_virtual_devices; /**< 虚拟设备数量 (1=不模拟, 2~8=模拟N台), 默认 1 */
    bool mqtt_enable_lwt;            /**< 启用 LWT, 默认 true */
    char mqtt_lwt_topic[64];         /**< LWT Topic, 默认 "edgevib/status" */
    bool mqtt_publish_vibration;     /**< 发布振动数据, 默认 true */
    bool mqtt_publish_environment;   /**< 发布环境数据, 默认 true */
    bool mqtt_publish_health;        /**< 发布健康状态, 默认 false */

    /* AI 配置 */
    float ai_anomaly_threshold;      /**< AI 异常分数阈值，默认 0.7 */

    /* 通信配置 */
    uint32_t heartbeat_interval_ms; /**< 心跳间隔 (ms)，默认 1000 */
    uint8_t device_id;              /**< 设备 ID，默认 1 */
    char device_name[32];           /**< 设备名称，默认 "EdgeVib-Sensor" */
    char gateway_url[128];          /**< 网关 URL，默认 "mqtt://gateway.local" */
    
    /* 安全配置 */
    bool encryption_enabled;        /**< 是否启用配置加密 */
    
    /* 系统配置 */
    bool auto_reboot_enabled;       /**< 是否启用自动重启 */
    uint32_t reboot_interval_seconds; /**< 自动重启间隔（秒），默认 86400（24小时）*/

    /* OTA 升级配置 */
    char ota_server_url[128];        /**< OTA 服务器基础 URL，默认 "http://firmware.example.com" */
    char ota_firmware_path[64];      /**< 固件路径，默认 "/edgevib/firmware.bin" */
    char ota_version_url[128];       /**< 版本检查 URL，默认 "http://firmware.example.com/version.json" */
    uint32_t ota_timeout_ms;         /**< HTTP 请求超时（毫秒），默认 30000 */
    uint32_t ota_buffer_size;        /**< 下载缓冲区大小（字节），默认 4096 */
    int ota_max_retries;             /**< 最大重试次数，默认 3 */
    bool ota_auto_check_enabled;     /**< 是否启用自动版本检查，默认 false */
    uint32_t ota_check_interval_s;   /**< 版本检查间隔（秒），默认 3600 (1小时)*/
    bool ota_verify_sha256;          /**< 是否验证固件 SHA256，默认 true */
    bool ota_rollback_on_failure;    /**< 升级失败时是否自动回滚，默认 true */

    /* 时间同步配置 */
    char timezone[16];               /**< 时区字符串，默认 "CST-8" (UTC+8) */
    char sntp_server1[64];           /**< SNTP 服务器1，默认 "pool.ntp.org" */
    char sntp_server2[64];           /**< SNTP 服务器2，默认 "time.google.com" */
    uint32_t sntp_sync_timeout_ms;   /**< SNTP 同步超时（毫秒），默认 10000 */
    uint32_t sntp_retry_interval_ms;/**< SNTP 重试间隔（毫秒），默认 5000 */
    int sntp_max_retries;            /**< SNTP 最大重试次数，默认 3 */
    bool sntp_auto_sync_enabled;    /**< 是否启用自动周期性同步，默认 true */
    uint32_t sntp_sync_interval_s;  /**< 自动同步间隔（秒），默认 3600 (1小时)*/
};

/* 配置变更回调函数类型 */
typedef void (*config_change_callback_t)(const char *key, const void *old_value, const void *new_value);

/* ==================== 生命周期管理 ==================== */

/**
 * config_manager_init - 初始化配置管理器，创建互斥量，从 NVS 加载配置
 *                  若无有效配置或版本不匹配则使用默认值并进行迁移
 * @cfg: 输出配置指针（可选，若为 NULL 则仅内部加载）
 *
 * Context: 必须在 FreeRTOS 调度启动前或任意任务中调用，不可在中断中调用
 * Return: APP_ERR_OK on success, negative error code on failure
 */
int config_manager_init(struct system_config *cfg);

/**
 * config_manager_deinit - 反初始化配置管理器，释放互斥量资源
 *                  调用后所有 get/set/save 操作将返回错误
 *
 * Context: 仅在任务上下文中调用
 * Return: APP_ERR_OK on success, negative error code on failure
 */
int config_manager_deinit(void);

/**
 * config_manager_is_initialized - 查询配置管理器是否已完成初始化
 *
 * Return: true 已初始化，false 未初始化
 */
bool config_manager_is_initialized(void);

/* ==================== 核心读写操作（线程安全） ==================== */

/**
 * config_manager_get - 获取当前配置快照（加锁读取，线程安全）
 *
 * 注意：返回的是内部配置的指针，调用者不应修改其内容。
 *       如需长时间持有配置副本，请使用 config_manager_export。
 *
 * Return: 指向当前配置的指针，未初始化时返回 NULL
 */
const struct system_config *config_manager_get(void);

/**
 * config_manager_set - 更新整组配置并立即持久化到 NVS（原子操作）
 * @cfg: 新配置（必须通过 validate 验证）
 *
 * 此函数内部完成：validate -> 加锁 -> 拷贝 -> 通知回调 -> 保存NV S-> 解锁
 *
 * Return: APP_ERR_OK on success, negative error code on failure
 */
int config_manager_set(const struct system_config *cfg);

/**
 * config_manager_save - 显式将当前内存配置持久化到 NVS
 *                  通常无需手动调用，set/reset 已包含自动保存
 *
 * Return: APP_ERR_OK on success, negative error code on failure
 */
int config_manager_save(void);

/**
 * config_manager_reset - 重置为默认配置并持久化
 *
 * Return: APP_ERR_OK on success, negative error code on failure
 */
int config_manager_reset(void);

/* ==================== 工厂重置 ==================== */

/**
 * config_manager_factory_reset - 完全擦除 NVS 配置命名空间并恢复出厂设置
 *                             同时清除所有备份槽位
 *
 * 典型场景：用户长按复位按钮、远程指令恢复出厂、固件首次部署
 *
 * Return: APP_ERR_OK on success, negative error code on failure
 */
int config_manager_factory_reset(void);

/* ==================== 单字段 Getter / Setter ==================== */

/**
 * config_manager_get_sample_rate - 获取采样率（线程安全）
 *
 * Return: 当前采样率 Hz，未初始化返回默认值 100
 */
int config_manager_get_sample_rate(void);

/**
 * config_manager_set_sample_rate - 设置采样率并自动保存
 * @rate: 新的采样率 (Hz)，范围 1~16000
 *
 * Return: APP_ERR_OK on success, negative error code on failure
 */
int config_manager_set_sample_rate(int rate);

/**
 * config_manager_get_fft_size - 获取 FFT 点数（线程安全）
 *
 * Return: 当前 FFT 点数，未初始化返回默认值 512
 */
int config_manager_get_fft_size(void);

/**
 * config_manager_set_fft_size - 设置 FFT 点数并自动保存
 * @size: 新的 FFT 点数（必须是 2 的幂，范围 64~4096）
 *
 * Return: APP_ERR_OK on success, negative error code on failure
 */
int config_manager_set_fft_size(int size);

/**
 * config_manager_get_device_id - 获取设备 ID（线程安全）
 *
 * Return: 当前设备 ID
 */
uint8_t config_manager_get_device_id(void);

/**
 * config_manager_get_rms_thresholds - 获取 RMS 阈值对（线程安全）
 * @warning_out: 输出警告阈值指针
 * @critical_out: 输出严重阈值指针
 *
 * Return: APP_ERR_OK on success, APP_ERR_INVALID_PARAM if output is NULL
 */
int config_manager_get_rms_thresholds(float *warning_out, float *critical_out);

/* ==================== ADXL345 配置 Getter/Setter ==================== */

/**
 * config_manager_get_adxl345_spi_config - 获取 ADXL345 SPI 完整配置
 * @host_id_out: 输出 SPI 主机 ID
 * @cs_out: 输出 CS 引脚
 * @miso_out: 输出 MISO 引脚
 * @mosi_out: 输出 MOSI 引脚
 * @sclk_out: 输出 SCLK 引脚
 * @clock_hz_out: 输出时钟频率
 */
void config_manager_get_adxl345_spi_config(int *host_id_out, int *cs_out,
                                            int *miso_out, int *mosi_out,
                                            int *sclk_out, int *clock_hz_out);

int config_manager_get_adxl345_range(void);
int config_manager_set_adxl345_range(int range);
int config_manager_get_adxl345_data_rate(void);
int config_manager_set_adxl345_data_rate(int rate);

/* ==================== DSP 配置 Getter/Setter ==================== */

int config_manager_get_dsp_window_type(void);
int config_manager_set_dsp_window_type(int window_type);
bool config_manager_get_dsp_dc_removal(void);
void config_manager_set_dsp_dc_removal(bool enable);

/* ==================== Temperature Compensation 配置 Getter/Setter ==================== */

bool config_manager_get_temp_comp_enabled(void);
void config_manager_set_temp_comp_enabled(bool enabled);
float config_manager_get_temp_comp_ewma_alpha(void);
int config_manager_set_temp_comp_ewma_alpha(float alpha);
float config_manager_get_temp_comp_change_threshold(void);
int config_manager_set_temp_comp_change_threshold(float threshold);

/* ==================== Sensor Service 配置 Getter/Setter ==================== */

uint32_t config_manager_get_analysis_interval(void);
int config_manager_set_analysis_interval(uint32_t interval_ms);
bool config_manager_get_sensor_protocol_temp(void);
void config_manager_set_sensor_protocol_temp(bool enable);


/* ==================== Protocol 配置 Getter/Setter ==================== */

int config_manager_get_protocol_uart_config(int *uart_num, int *baud_rate,
                                             int *tx_pin, int *rx_pin);
bool config_manager_get_protocol_ack_enabled(void);
uint32_t config_manager_get_protocol_heartbeat_interval(void);

/* ==================== 导出 / 导入（序列化） ==================== */

/**
 * config_manager_export - 将当前配置序列化为带 CRC 的字节流
 * @buf: 输出缓冲区，大小至少 CONFIG_EXPORT_BUF_SIZE 字节
 * @buf_len: 缓冲区容量
 * @out_actual_len: 实际写入的字节数
 *
 * 输出格式: [struct system_config (raw bytes)] [uint32_t crc32 (little-endian)]
 * 用途：远程配置同步、OTA 批量部署、调试导出
 *
 * Return: APP_ERR_OK on success, negative error code on failure
 */
int config_manager_export(uint8_t *buf, size_t buf_len, size_t *out_actual_len);

/**
 * config_manager_import - 从字节流导入配置（含 CRC 验证），验证通过后自动保存
 * @buf: 输入缓冲区（由 export 生成或符合相同格式）
 * @len: 输入数据长度
 *
 * 流程：CRC 校验 -> 版本检查 -> 字段验证 -> 加锁替换 -> 通知回调 -> NVS 持久化
 *
 * Return: APP_ERR_OK on success, negative error code on failure
 */
int config_manager_import(const uint8_t *buf, size_t len);

/* ==================== 校验与版本 ==================== */

/**
 * config_manager_validate - 验证配置所有字段的合法性和取值范围
 * @cfg: 要验证的配置指针
 *
 * Return: APP_ERR_OK on success, APP_ERR_CONFIG_VALIDATION on validation failure
 */
int config_manager_validate(const struct system_config *cfg);

/**
 * config_manager_get_version - 获取当前加载的配置版本号
 *
 * Return: 配置版本号 (CONFIG_VERSION 格式)
 */
uint32_t config_manager_get_version(void);

/**
 * config_manager_compute_crc32 - 计算配置数据的 CRC32 校验和
 * @cfg: 配置指针
 *
 * Return: CRC32 值
 */
uint32_t config_manager_compute_crc32(const struct system_config *cfg);

/* ==================== 备份与恢复 ==================== */

/**
 * config_manager_backup - 将当前配置备份到指定 NVS 槽位
 * @backup_slot: 备份槽位编号 (0 ~ CONFIG_MAX_BACKUP_SLOTS-1)
 *
 * Return: APP_ERR_OK on success, negative error code on failure
 */
int config_manager_backup(uint8_t backup_slot);

/**
 * config_manager_restore - 从指定 NVS 槽位恢复配置（含验证与回调通知）
 * @backup_slot: 备份槽位编号 (0 ~ CONFIG_MAX_BACKUP_SLOTS-1)
 *
 * Return: APP_ERR_OK on success, negative error code on failure
 */
int config_manager_restore(uint8_t backup_slot);

/* ==================== 变更回调 ==================== */

/**
 * config_manager_register_callback - 注册配置变更监听回调
 * @callback: 回调函数指针
 *
 * 最多注册 MAX_CALLBACKS 个回调，重复注册会被忽略。
 * 回调在 set/reset/restore/import 操作的临界区内调用。
 *
 * Return: APP_ERR_OK on success, negative error code on failure
 */
int config_manager_register_callback(config_change_callback_t callback);

/**
 * config_manager_unregister_callback - 注销已注册的配置变更回调
 * @callback: 待注销的回调函数指针
 *
 * Return: APP_ERR_OK on success, negative error code on failure
 */
int config_manager_unregister_callback(config_change_callback_t callback);

#endif /* CONFIG_MANAGER_H_ */