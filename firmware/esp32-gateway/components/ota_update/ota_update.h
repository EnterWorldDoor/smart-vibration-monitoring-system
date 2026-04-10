/**
 * @file ota_update.h
 * @author EnterWorldDoor
 * @brief 企业级 OTA 升级模块：HTTP 下载、SHA256 校验、版本检查、回滚、状态机、回调通知
 *
 * 功能特性:
 *   - 基于 ESP-IDF esp_http_client + esp_ota 的完整 OTA 流程
 *   - SHA256 固件完整性验证（可选）
 *   - 版本检查与比较机制
 *   - A/B 分区回滚支持
 *   - 状态机管理 (IDLE → DOWNLOADING → VERIFYING → COMPLETE/FAILED)
 *   - 多级回调通知系统（进度/状态变更/完成/错误）
 *   - 可配置的重试机制
 *   - FreeRTOS Mutex 线程安全
 *   - 取消/中止操作支持
 *   - 与 config_manager 集成获取配置参数
 */

#ifndef OTA_UPDATE_H_
#define OTA_UPDATE_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "global_error.h"

/* ==================== 配置常量 ==================== */

#define OTA_MAX_URL_LEN            512      /**< 最大 URL 长度 */
#define OTA_DEFAULT_BUF_SIZE       4096     /**< 默认下载缓冲区大小（字节）*/
#define OTA_DEFAULT_TIMEOUT_MS     30000    /**< 默认 HTTP 超时（毫秒）*/
#define OTA_DEFAULT_MAX_RETRIES    3        /**< 默认最大重试次数 */
#define OTA_TASK_STACK_SIZE        8192     /**< OTA 任务栈大小（字节，需较大空间处理 TLS）*/
#define OTA_TASK_PRIORITY          10       /**< OTA 任务优先级 */
#define OTA_MAX_CALLBACKS          4        /**< 最大回调注册数 */
#define OTA_SHA256_HEX_LEN         65       /**< SHA256 十六进制字符串长度（含 NULL）*/
#define OTA_VERSION_STR_LEN        32       /**< 固件版本字符串最大长度 */

/* ==================== 状态机枚举 ==================== */

enum ota_state {
    OTA_STATE_IDLE = 0,             /**< 空闲，可开始新升级 */
    OTA_STATE_CHECKING_VERSION,     /**< 正在检查服务器版本 */
    OTA_STATE_DOWNLOADING,          /**< 正在下载固件 */
    OTA_STATE_VERIFYING,            /**< 正在验证固件完整性 */
    OTA_STATE_WRITING,              /**< 正在写入分区 */
    OTA_STATE_COMPLETED,            /**< 升级成功完成 */
    OTA_STATE_FAILED,               /**< 升级失败 */
    OTA_STATE_ROLLBACK,             /**< 正在回滚 */
    OTA_STATE_ABORTED               /**< 被用户中止 */
};

/* ==================== 固件元数据结构 ==================== */

/**
 * struct ota_firmware_info - 从服务器获取的固件元数据
 */
struct ota_firmware_info {
    char version[OTA_VERSION_STR_LEN];   /**< 固件版本号（如 "1.2.3"）*/
    uint32_t size;                       /**< 固件大小（字节）*/
    char sha256[OTA_SHA256_HEX_LEN];     /**< SHA256 校验值（十六进制字符串）*/
    char release_notes[128];             /**< 发布说明 */
    char url[OTA_MAX_URL_LEN];           /**< 完整下载 URL */
    bool force_update;                   /**< 是否强制更新（跳过版本比较）*/
};

/**
 * struct ota_progress - 进度信息快照
 */
struct ota_progress {
    int percent;                         /**< 总进度百分比 (0~100) */
    size_t bytes_downloaded;             /**< 已下载字节数 */
    size_t bytes_total;                 /**< 总字节数（0=未知）*/
    enum ota_state state;                /**< 当前状态 */
    int retry_count;                     /**< 当前重试次数 */
};

/**
 * struct ota_result - 升级结果
 */
struct ota_result {
    enum ota_state final_state;           /**< 最终状态 */
    int error_code;                      /**< 错误码（成功为 APP_ERR_OK）*/
    char error_msg[64];                  /**< 错误描述 */
    struct ota_firmware_info firmware;   /**< 已安装的固件信息 */
    uint32_t total_time_ms;              /**< 总耗时（毫秒）*/
};

/* ==================== 回调函数类型 ==================== */

/**
 * ota_progress_cb_t - 下载进度回调
 * @progress: 当前进度信息指针
 * @user_data: 用户自定义上下文
 *
 * 注意：此回调在 OTA 任务上下文中调用，不应执行耗时操作
 */
typedef void (*ota_progress_cb_t)(const struct ota_progress *progress,
                                  void *user_data);

/**
 * ota_state_change_cb_t - 状态变更回调
 * @old_state: 前一个状态
 * @new_state: 新状态
 * @error_code: 如果是 FAILED/ABORTED 状态，包含错误码
 * @user_data: 用户自定义上下文
 */
typedef void (*ota_state_change_cb_t)(enum ota_state old_state,
                                      enum ota_state new_state,
                                      int error_code,
                                      void *user_data);

/**
 * ota_complete_cb_t - 升级完成回调（无论成功或失败都会触发）
 * @result: 结果详情指针
 * @user_data: 用户自定义上下文
 */
typedef void (*ota_complete_cb_t)(const struct ota_result *result,
                                  void *user_data);

/* ==================== 生命周期管理 ==================== */

/**
 * ota_update_init - 初始化 OTA 模块
 *                  创建互斥量，从 config_manager 读取配置参数
 *
 * Return: APP_ERR_OK on success, negative error code on failure
 */
int ota_update_init(void);

/**
 * ota_update_deinit - 反初始化 OTA 模块，释放资源
 *                  若有正在进行的升级操作会被中止
 *
 * Return: APP_ERR_OK on success, negative error code on failure
 */
int ota_update_deinit(void);

/**
 * ota_update_is_initialized - 查询是否已初始化
 *
 * Return: true 已初始化，false 未初始化
 */
bool ota_update_is_initialized(void);

/**
 * ota_update_is_busy - 查询是否有正在进行的升级操作
 *
 * Return: true 正在升级中，false 空闲
 */
bool ota_update_is_busy(void);

/* ==================== 核心升级 API ==================== */

/**
 * ota_update_start - 启动 OTA 升级（阻塞模式，当前任务执行）
 * @url: 固件下载 URL（NULL 则使用 config_manager 中配置的默认 URL）
 * @expected_sha256: 预期的 SHA256 值（NULL 则不校验或使用配置中的值）
 *
 * 此函数会阻塞直到升级完成或失败。
 * 完整流程：HTTP 连接 → 下载 → 写入分区 → 验证 → 设置启动分区 → 重启
 *
 * Return: APP_ERR_OK on success, negative error code on failure
 */
int ota_update_start(const char *url, const char *expected_sha256);

/**
 * ota_update_start_async - 异步启动 OTA 升级（创建独立后台任务执行）
 * @url: 固件下载 URL（NULL 使用默认）
 * @expected_sha256: 预期 SHA256（NULL 不校验）
 * @complete_cb: 升级完成回调（可选）
 * @user_data: 回调用户上下文
 *
 * Return: APP_ERR_OK on success (仅表示任务已创建), negative on failure
 */
int ota_update_start_async(const char *url, const char *expected_sha256,
                           ota_complete_cb_t complete_cb, void *user_data);

/**
 * ota_update_abort - 中止正在进行的升级操作
 *
 * Return: APP_ERR_OK on success, negative error code on failure
 */
int ota_update_abort(void);

/* ==================== 版本检查 ==================== */

/**
 * ota_update_check_version - 检查服务器是否有新版本可用
 * @version_url: 版本 JSON 接口 URL（NULL 使用配置中的默认值）
 * @current_version: 当前固件版本字符串（如 "1.2.3"）
 * @out_info: 输出：新版本固件元数据（如有更新）
 *
 * Return: APP_ERR_OK 有新版本, APP_ERR_NOT_SUPPORTED 无需更新, negative on error
 */
int ota_update_check_version(const char *version_url,
                             const char *current_version,
                             struct ota_firmware_info *out_info);

/**
 * ota_update_compare_versions - 比较两个版本号字符串
 * @v1: 版本号1（如 "1.2.3"）
 * @v2: 版本号2
 *
 * Return: >0 if v1 > v2, ==0 if equal, <0 if v1 < v2
 */
int ota_update_compare_versions(const char *v1, const char *v2);

/* ==================== 回滚 ==================== */

/**
 * ota_update_rollback - 回滚到上一个有效分区
 * @reboot: 是否自动重启以应用回滚
 *
 * Return: APP_ERR_OK on success, negative error code on failure
 */
int ota_update_rollback(bool reboot);

/**
 * ota_update_mark_current_valid - 标记当前运行的固件为已确认有效
 *                                 （防止自动回滚）
 *
 * Return: APP_ERR_OK on success, negative error code on failure
 */
int ota_update_mark_current_valid(void);

/* ==================== 状态查询 ==================== */

/**
 * ota_update_get_state - 获取当前 OTA 状态机状态
 *
 * Return: 当前状态枚举值
 */
enum ota_state ota_update_get_state(void);

/**
 * ota_update_get_progress - 获取当前升级进度快照
 * @out: 输出进度结构体指针
 *
 * Return: APP_ERR_OK or error code
 */
int ota_update_get_progress(struct ota_progress *out);

/**
 * ota_update_get_running_partition_info - 获取当前运行分区信息
 * @partition_label: 输出分区标签缓冲区（至少 17 字节）
 * @label_size: 缓冲区大小
 * @app_version: 输出应用版本号缓冲区（至少 33 字节）
 * @ver_size: 版本缓冲区大小
 *
 * Return: APP_ERR_OK or error code
 */
int ota_update_get_running_partition_info(char *partition_label, size_t label_size,
                                         char *app_version, size_t ver_size);

/* ==================== 回调注册 ==================== */

/**
 * ota_update_register_progress_callback - 注册进度回调
 * @callback: 回调函数
 * @user_data: 用户上下文
 *
 * Return: APP_ERR_OK or error code
 */
int ota_update_register_progress_callback(ota_progress_cb_t callback,
                                          void *user_data);

/**
 * ota_update_register_state_callback - 注册状态变更回调
 * @callback: 回调函数
 * @user_data: 用户上下文
 *
 * Return: APP_ERR_OK or error code
 */
int ota_update_register_state_callback(ota_state_change_cb_t callback,
                                       void *user_data);

/**
 * ota_update_unregister_progress_callback - 注销进度回调
 * @callback: 待注销的回调函数
 *
 * Return: APP_ERR_OK or error code
 */
int ota_update_unregister_progress_callback(ota_progress_cb_t callback);

/**
 * ota_update_unregister_state_callback - 注销状态变更回调
 * @callback: 待注销的回调函数
 *
 * Return: APP_ERR_OK or error code
 */
int ota_update_unregister_state_callback(ota_state_change_cb_t callback);

#endif /* OTA_UPDATE_H_ */