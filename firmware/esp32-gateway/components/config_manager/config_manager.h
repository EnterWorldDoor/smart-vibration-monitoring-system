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
    
    /* 传感器配置 */
    int sample_rate_hz;          /**< 采样率 (Hz)，默认 100 */
    int sensor_buffer_size;      /**< 传感器环形缓冲区大小（样本数），默认 512 */

    /* DSP 配置 */
    int fft_size;                /**< FFT 点数，默认 512（2 的幂）*/
    float rms_warning_threshold; /**< RMS 警告阈值 (g)，默认 2.0 */
    float rms_critical_threshold;/**< RMS 严重阈值 (g)，默认 4.0 */
    float freq_peak_threshold;   /**< 频谱峰值异常阈值，默认 0.5 */

    /* AI 配置 */
    float ai_anomaly_threshold;  /**< AI 异常分数阈值，默认 0.7 */

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