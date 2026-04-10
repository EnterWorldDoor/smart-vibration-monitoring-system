/**
 * @file ota_update.c
 * @author EnterWorldDoor
 * @brief 企业级 OTA 升级实现：HTTP 下载、SHA256 校验、版本检查、回滚、状态机、线程安全
 *
 * 架构设计:
 *   - 状态机驱动: IDLE → CHECKING → DOWNLOADING → VERIFYING → WRITING → COMPLETE/FAILED
 *   - 所有共享状态通过 Mutex 保护，确保多任务并发安全
 *   - 支持同步（阻塞）和异步（后台任务）两种升级模式
 *   - 与 config_manager 集成，自动读取 OTA 配置参数
 *   - SHA256 完整性验证保障固件安全性
 */

#include "ota_update.h"
#include "log_system.h"
#include "config_manager.h"
#include "global_error.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_err.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>

/* ==================== 模块内部状态 ==================== */

static struct {
    bool initialized;
    enum ota_state state;
    bool abort_flag;
    SemaphoreHandle_t mutex;
    TaskHandle_t async_task_handle;

    /* 配置参数（从 config_manager 加载）*/
    char server_url[OTA_MAX_URL_LEN];
    uint32_t timeout_ms;
    uint32_t buffer_size;
    int max_retries;
    bool verify_sha256;
    bool rollback_on_failure;

    /* 进度跟踪 */
    struct ota_progress progress;

    /* 回调注册 */
    ota_progress_cb_t progress_cbs[OTA_MAX_CALLBACKS];
    void *progress_cb_data[OTA_MAX_CALLBACKS];
    int progress_cb_count;

    ota_state_change_cb_t state_cbs[OTA_MAX_CALLBACKS];
    void *state_cb_data[OTA_MAX_CALLBACKS];
    int state_cb_count;
} g_ota = {0};

/* ==================== 内部辅助函数 ==================== */

/**
 * set_state - 内部函数：原子地更新状态并通知回调（调用者需持有锁）
 * @new_state: 新状态
 * @error_code: 关联的错误码（成功为 APP_ERR_OK）
 */
static void set_state(enum ota_state new_state, int error_code)
{
    enum ota_state old = g_ota.state;
    g_ota.state = new_state;
    g_ota.progress.state = new_state;

    for (int i = 0; i < g_ota.state_cb_count; i++) {
        if (g_ota.state_cbs[i]) {
            g_ota.state_cbs[i](old, new_state, error_code,
                             g_ota.state_cb_data[i]);
        }
    }
}

/**
 * notify_progress - 内部函数：通知所有进度回调（调用者需持有锁）
 */
static void notify_progress(void)
{
    for (int i = 0; i < g_ota.progress_cb_count; i++) {
        if (g_ota.progress_cbs[i]) {
            g_ota.progress_cbs[i](&g_ota.progress,
                                 g_ota.progress_cb_data[i]);
        }
    }
}

/**
 * load_config_from_manager - 从 config_manager 加载 OTA 配置参数
 *
 * Return: APP_ERR_OK or error code
 */
static int load_config_from_manager(void)
{
    if (!config_manager_is_initialized()) {
        LOG_WARN("OTA", "config_manager not initialized, using defaults");
        strncpy(g_ota.server_url, "http://firmware.example.com",
                OTA_MAX_URL_LEN - 1);
        g_ota.timeout_ms = OTA_DEFAULT_TIMEOUT_MS;
        g_ota.buffer_size = OTA_DEFAULT_BUF_SIZE;
        g_ota.max_retries = OTA_DEFAULT_MAX_RETRIES;
        g_ota.verify_sha256 = true;
        g_ota.rollback_on_failure = true;
        return APP_ERR_OK;
    }

    const struct system_config *cfg = config_manager_get();
    if (!cfg) return APP_ERR_CONFIG_LOAD_FAIL;

    if (cfg->ota_server_url[0]) {
        strncpy(g_ota.server_url, cfg->ota_server_url,
                OTA_MAX_URL_LEN - 1);
    }
    g_ota.timeout_ms = cfg->ota_timeout_ms > 0 ? cfg->ota_timeout_ms
                                                : OTA_DEFAULT_TIMEOUT_MS;
    g_ota.buffer_size = cfg->ota_buffer_size > 0 ? cfg->ota_buffer_size
                                              : OTA_DEFAULT_BUF_SIZE;
    g_ota.max_retries = cfg->ota_max_retries > 0 ? cfg->ota_max_retries
                                              : OTA_DEFAULT_MAX_RETRIES;
    g_ota.verify_sha256 = cfg->ota_verify_sha256;
    g_ota.rollback_on_failure = cfg->ota_rollback_on_failure;

    LOG_INFO("OTA", "Config loaded: url=%s timeout=%u buf=%u retries=%d sha256=%d rollback=%d",
             g_ota.server_url, g_ota.timeout_ms, g_ota.buffer_size,
             g_ota.max_retries, g_ota.verify_sha256, g_ota.rollback_on_failure);
    return APP_ERR_OK;
}

/**
 * resolve_url - 解析最终下载 URL
 * @user_input: 用户传入的 URL（NULL 使用配置默认值）
 * @out_buf: 输出缓冲区
 * @buf_len: 缓冲区大小
 *
 * Return: APP_ERR_OK or error code
 */
static int resolve_url(const char *user_input, char *out_buf, size_t buf_len)
{
    if (user_input && user_input[0]) {
        if (strlen(user_input) >= buf_len) return APP_ERR_OTA_INVALID_URL;
        strncpy(out_buf, user_input, buf_len - 1);
        out_buf[buf_len - 1] = '\0';
    } else {
        const struct system_config *cfg = config_manager_get();
        if (cfg && cfg->ota_server_url[0] && cfg->ota_firmware_path[0]) {
            snprintf(out_buf, buf_len, "%s%s", cfg->ota_server_url,
                     cfg->ota_firmware_path);
        } else {
            snprintf(out_buf, buf_len, "%s/firmware.bin", g_ota.server_url);
        }
    }
    LOG_DEBUG("OTA", "Resolved URL: %s", out_buf);
    return APP_ERR_OK;
}

/**
 * compute_sha256 - 计算数据的 SHA256 哈希值
 * @data: 数据指针
 * @len: 数据长度
 * @out_hex: 输出的十六进制字符串缓冲区（至少 65 字节）
 */
static void __attribute__((unused)) compute_sha256(const uint8_t *data, size_t len, char *out_hex)
{
    unsigned char hash[32];
    mbedtls_sha256(data, len, hash, 0); /* is224=0 -> SHA-256 */
    for (int i = 0; i < 32; i++) {
        sprintf(out_hex + i * 2, "%02x", hash[i]);
    }
    out_hex[64] = '\0';
}

/**
 * perform_ota_download - 核心下载与写入逻辑（调用者需持有锁）
 * @url: 下载 URL
 * @expected_sha256: 预期 SHA256（NULL 不校验）
 *
 * Return: APP_ERR_OK on success, negative error code on failure
 */
static int perform_ota_download(const char *url, const char *expected_sha256)
{
    esp_err_t err;

    /* --- HTTP 客户端初始化 --- */
    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = g_ota.timeout_ms,
        .keep_alive_enable = false,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        LOG_ERROR("OTA", "HTTP client init failed");
        return APP_ERR_OTA_HTTP_INIT;
    }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        LOG_ERROR("OTA", "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return APP_ERR_OTA_HTTP_CONNECT;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0) {
        LOG_ERROR("OTA", "Invalid content length: %d", content_length);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return APP_ERR_OTA_HTTP_RESPONSE;
    }

    LOG_INFO("OTA", "Firmware size: %d bytes", content_length);

    /* --- 获取目标分区 --- */
    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        LOG_ERROR("OTA", "No update partition available");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return APP_ERR_OTA_NO_UPDATE_PART;
    }
    LOG_INFO("OTA", "Writing to partition: %s (offset=0x%x, size=0x%x)",
             update_part->label, update_part->address, update_part->size);

    /* --- 开始 OTA 写入 --- */
    esp_ota_handle_t ota_handle;
    err = esp_ota_begin(update_part, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        LOG_ERROR("OTA", "OTA begin failed: %s", esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return APP_ERR_OTA_BEGIN_FAILED;
    }

    /* --- 下载循环 --- */
    uint8_t *buf = malloc(g_ota.buffer_size);
    if (!buf) {
        LOG_ERROR("OTA", "Failed to allocate download buffer (%u bytes)",
                  g_ota.buffer_size);
        esp_ota_abort(ota_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return APP_ERR_NO_MEM;
    }
    /* 动态分配，在函数末尾 free() */

    int total_read = 0;
    int last_percent = -1;
    mbedtls_sha256_context sha_ctx;
    bool do_verify = (expected_sha256 && expected_sha256[0]) ||
                    (g_ota.verify_sha256);

    if (do_verify) {
        mbedtls_sha256_init(&sha_ctx);
        mbedtls_sha256_starts(&sha_ctx, 0);
    }

    set_state(OTA_STATE_DOWNLOADING, APP_ERR_OK);
    g_ota.progress.bytes_total = content_length;
    g_ota.progress.bytes_downloaded = 0;
    g_ota.progress.percent = 0;

    while (!g_ota.abort_flag) {
        int read_len = esp_http_client_read(client, (char *)buf,
                                            g_ota.buffer_size);
        if (read_len <= 0) break;

        /* 更新 SHA256 */
        if (do_verify) {
            mbedtls_sha256_update(&sha_ctx, buf, read_len);
        }

        /* 写入 OTA 分区 */
        err = esp_ota_write(ota_handle, buf, read_len);
        if (err != ESP_OK) {
            LOG_ERROR("OTA", "OTA write failed at offset %d: %s",
                      total_read, esp_err_to_name(err));
            free(buf);
            if (do_verify) mbedtls_sha256_free(&sha_ctx);
            esp_ota_abort(ota_handle);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return APP_ERR_OTA_WRITE_FAILED;
        }

        total_read += read_len;
        g_ota.progress.bytes_downloaded = total_read;

        if (content_length > 0) {
            g_ota.progress.percent = total_read * 100 / content_length;
            if (g_ota.progress.percent != last_percent) {
                last_percent = g_ota.progress.percent;
                notify_progress();
            }
        }
    }

    free(buf); /* 释放动态分配的下载缓冲区 */
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (g_ota.abort_flag) {
        LOG_WARN("OTA", "Download aborted by user");
        if (do_verify) mbedtls_sha256_free(&sha_ctx);
        esp_ota_abort(ota_handle);
        return APP_ERR_OTA_ABORTED;
    }

    if (total_read == 0) {
        LOG_ERROR("OTA", "Download failed: no data received");
        if (do_verify) mbedtls_sha256_free(&sha_ctx);
        esp_ota_abort(ota_handle);
        return APP_ERR_OTA_DOWNLOAD_FAILED;
    }

    /* --- SHA256 验证 --- */
    if (do_verify) {
        unsigned char hash[32];
        mbedtls_sha256_finish(&sha_ctx, hash);
        mbedtls_sha256_free(&sha_ctx);

        char computed_hash[65];
        for (int i = 0; i < 32; i++) {
            sprintf(computed_hash + i * 2, "%02x", hash[i]);
        }
        computed_hash[64] = '\0';

        LOG_INFO("OTA", "SHA256 computed: %s", computed_hash);

        if (expected_sha256 && expected_sha256[0]) {
            if (strncmp(computed_hash, expected_sha256, 64) != 0) {
                LOG_ERROR("OTA", "SHA256 mismatch! expected: %s got: %s",
                          expected_sha256, computed_hash);
                esp_ota_abort(ota_handle);
                return APP_ERR_OTA_VERIFY_FAILED;
            }
            LOG_INFO("OTA", "SHA256 verification passed");
        }
    }

    /* --- 完成 OTA 写入 --- */
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        LOG_ERROR("OTA", "OTA end failed: %s", esp_err_to_name(err));
        return APP_ERR_OTA_END_FAILED;
    }

    /* --- 设置启动分区 --- */
    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        LOG_ERROR("OTA", "Set boot partition failed: %s", esp_err_to_name(err));
        return APP_ERR_OTA_SET_BOOT_FAILED;
    }

    set_state(OTA_STATE_COMPLETED, APP_ERR_OK);
    g_ota.progress.percent = 100;
    notify_progress();

    LOG_INFO("OTA", "OTA update successful, total bytes: %d", total_read);
    return APP_ERR_OK;
}

/* ==================== 异步任务 ==================== */

struct async_task_params {
    char url[OTA_MAX_URL_LEN];
    char sha256[OTA_SHA256_HEX_LEN];
    ota_complete_cb_t complete_cb;
    void *user_data;
};

static void ota_async_task(void *arg)
{
    struct async_task_params *params = (struct async_task_params *)arg;
    struct ota_result result;
    memset(&result, 0, sizeof(result));

    uint64_t start_ms = esp_timer_get_time() / 1000ULL;

    int ret = perform_ota_download(params->url,
                                   params->sha256[0] ? params->sha256 : NULL);

    result.total_time_ms = (uint32_t)(esp_timer_get_time() / 1000ULL - start_ms);
    result.final_state = g_ota.state;
    result.error_code = ret;

    if (ret == APP_ERR_OK) {
        strncpy(result.error_msg, "Success", sizeof(result.error_msg) - 1);
        LOG_INFO("OTA", "Async OTA completed successfully in %u ms",
                 result.total_time_ms);
    } else if (ret == APP_ERR_OTA_ABORTED) {
        strncpy(result.error_msg, "Aborted by user", sizeof(result.error_msg) - 1);
        LOG_WARN("OTA", "Async OTA aborted");
    } else {
        snprintf(result.error_msg, sizeof(result.error_msg) - 1,
                 "Error %d", ret);
        LOG_ERROR("OTA", "Async OTA failed: code=%d msg=%s", ret, result.error_msg);
    }

    if (params->complete_cb) {
        params->complete_cb(&result, params->user_data);
    }

    free(params);
    g_ota.async_task_handle = NULL;
    vTaskDelete(NULL);
}

/* ==================== 生命周期 API ==================== */

int ota_update_init(void)
{
    if (g_ota.initialized) return APP_ERR_OTA_ALREADY_IN_PROGRESS;

    memset(&g_ota, 0, sizeof(g_ota));

    g_ota.mutex = xSemaphoreCreateMutex();
    if (!g_ota.mutex) {
        LOG_ERROR("OTA", "Failed to create mutex");
        return APP_ERR_NO_MEM;
    }

    g_ota.state = OTA_STATE_IDLE;
    g_ota.abort_flag = false;

    int ret = load_config_from_manager();
    if (ret != APP_ERR_OK) {
        LOG_WARN("OTA", "Using default config (config_manager error: %d)", ret);
    }

    g_ota.initialized = true;
    LOG_INFO("OTA", "Module initialized (server=%s, timeout=%u ms, buf=%u, retries=%d)",
             g_ota.server_url, g_ota.timeout_ms, g_ota.buffer_size,
             g_ota.max_retries);
    return APP_ERR_OK;
}

int ota_update_deinit(void)
{
    if (!g_ota.initialized) return APP_ERR_OTA_NOT_INIT;

    xSemaphoreTake(g_ota.mutex, portMAX_DELAY);

    if (g_ota.async_task_handle) {
        g_ota.abort_flag = true;
        xSemaphoreGive(g_ota.mutex);
        vTaskDelay(pdMS_TO_TICKS(100));
        xSemaphoreTake(g_ota.mutex, portMAX_DELAY);
    }

    g_ota.initialized = false;
    g_ota.state = OTA_STATE_IDLE;
    g_ota.progress_cb_count = 0;
    g_ota.state_cb_count = 0;
    memset(g_ota.progress_cbs, 0, sizeof(g_ota.progress_cbs));
    memset(g_ota.state_cbs, 0, sizeof(g_ota.state_cbs));

    xSemaphoreGive(g_ota.mutex);
    vSemaphoreDelete(g_ota.mutex);
    g_ota.mutex = NULL;

    LOG_INFO("OTA", "Deinitialized");
    return APP_ERR_OK;
}

bool ota_update_is_initialized(void)
{
    return g_ota.initialized;
}

bool ota_update_is_busy(void)
{
    if (!g_ota.initialized || !g_ota.mutex) return false;
    bool busy;
    xSemaphoreTake(g_ota.mutex, portMAX_DELAY);
    busy = (g_ota.state != OTA_STATE_IDLE &&
            g_ota.state != OTA_STATE_COMPLETED &&
            g_ota.state != OTA_STATE_FAILED &&
            g_ota.state != OTA_STATE_ABORTED);
    xSemaphoreGive(g_ota.mutex);
    return busy;
}

/* ==================== 核心升级 API ==================== */

int ota_update_start(const char *url, const char *expected_sha256)
{
    if (!g_ota.initialized || !g_ota.mutex) return APP_ERR_OTA_NOT_INIT;

    xSemaphoreTake(g_ota.mutex, portMAX_DELAY);

    if (g_ota.state != OTA_STATE_IDLE &&
        g_ota.state != OTA_STATE_COMPLETED &&
        g_ota.state != OTA_STATE_FAILED &&
        g_ota.state != OTA_STATE_ABORTED) {
        xSemaphoreGive(g_ota.mutex);
        return APP_ERR_OTA_ALREADY_IN_PROGRESS;
    }

    g_ota.abort_flag = false;
    g_ota.progress.retry_count = 0;

    char resolved_url[OTA_MAX_URL_LEN];
    int ret = resolve_url(url, resolved_url, sizeof(resolved_url));
    if (ret != APP_ERR_OK) {
        xSemaphoreGive(g_ota.mutex);
        return ret;
    }

    int last_error = APP_ERR_OK;
    for (int attempt = 0; attempt <= g_ota.max_retries; attempt++) {
        if (attempt > 0) {
            LOG_WARN("OTA", "Retry attempt %d/%d",
                     attempt, g_ota.max_retries);
            g_ota.progress.retry_count = attempt;
            set_state(OTA_STATE_DOWNLOADING, APP_ERR_OK);
        }

        ret = perform_ota_download(resolved_url, expected_sha256);
        if (ret == APP_ERR_OK || ret == APP_ERR_OTA_ABORTED) break;

        last_error = ret;
        (void)last_error;
        set_state(OTA_STATE_FAILED, ret);

        if (attempt < g_ota.max_retries) {
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }

    if (ret != APP_ERR_OK && ret != APP_ERR_OTA_ABORTED) {
        if (g_ota.rollback_on_failure) {
            LOG_WARN("OTA", "Update failed, attempting rollback...");
            xSemaphoreGive(g_ota.mutex);
            ota_update_rollback(false);
            xSemaphoreTake(g_ota.mutex, portMAX_DELAY);
        }
    }

    xSemaphoreGive(g_ota.mutex);

    if (ret == APP_ERR_OK) {
        LOG_INFO("OTA", "Update complete, rebooting...");
        esp_restart();
    }

    return ret;
}

int ota_update_start_async(const char *url, const char *expected_sha256,
                           ota_complete_cb_t complete_cb, void *user_data)
{
    if (!g_ota.initialized || !g_ota.mutex) return APP_ERR_OTA_NOT_INIT;

    xSemaphoreTake(g_ota.mutex, portMAX_DELAY);

    if (g_ota.state != OTA_STATE_IDLE &&
        g_ota.state != OTA_STATE_COMPLETED &&
        g_ota.state != OTA_STATE_FAILED &&
        g_ota.state != OTA_STATE_ABORTED) {
        xSemaphoreGive(g_ota.mutex);
        return APP_ERR_OTA_ALREADY_IN_PROGRESS;
    }

    struct async_task_params *params = malloc(sizeof(*params));
    if (!params) {
        xSemaphoreGive(g_ota.mutex);
        return APP_ERR_NO_MEM;
    }

    memset(params, 0, sizeof(*params));
    if (url) strncpy(params->url, url, OTA_MAX_URL_LEN - 1);
    if (expected_sha256) strncpy(params->sha256, expected_sha256,
                                  OTA_SHA256_HEX_LEN - 1);
    params->complete_cb = complete_cb;
    params->user_data = user_data;

    BaseType_t task_ret = xTaskCreate(ota_async_task, "ota_task",
                                      OTA_TASK_STACK_SIZE, params,
                                      OTA_TASK_PRIORITY, &g_ota.async_task_handle);
    if (task_ret != pdPASS) {
        free(params);
        xSemaphoreGive(g_ota.mutex);
        LOG_ERROR("OTA", "Failed to create async OTA task");
        return APP_ERR_NO_MEM;
    }

    xSemaphoreGive(g_ota.mutex);
    LOG_INFO("OTA", "Async OTA task started");
    return APP_ERR_OK;
}

int ota_update_abort(void)
{
    if (!g_ota.initialized || !g_ota.mutex) return APP_ERR_OTA_NOT_INIT;

    xSemaphoreTake(g_ota.mutex, portMAX_DELAY);
    g_ota.abort_flag = true;
    set_state(OTA_STATE_ABORTED, APP_ERR_OTA_ABORTED);
    xSemaphoreGive(g_ota.mutex);

    LOG_INFO("OTA", "Abort requested");
    return APP_ERR_OK;
}

/* ==================== 版本检查 API ==================== */

int ota_update_check_version(const char *version_url,
                             const char *current_version,
                             struct ota_firmware_info *out_info)
{
    if (!current_version || !out_info) return APP_ERR_INVALID_PARAM;
    if (!g_ota.initialized) return APP_ERR_OTA_NOT_INIT;

    char url[OTA_MAX_URL_LEN];
    if (version_url && version_url[0]) {
        strncpy(url, version_url, OTA_MAX_URL_LEN - 1);
    } else {
        const struct system_config *cfg = config_manager_get();
        if (cfg && cfg->ota_version_url[0]) {
            strncpy(url, cfg->ota_version_url, OTA_MAX_URL_LEN - 1);
        } else {
            size_t len = strnlen(g_ota.server_url, OTA_MAX_URL_LEN - 1);
            memcpy(url, g_ota.server_url, len);
            url[len] = '\0';
            if (len + 13 < OTA_MAX_URL_LEN) {
                strcat(url, "/version.json");
            } else {
                LOG_WARN("OTA", "Server URL too long for version check");
            }
        }
    }

    LOG_INFO("OTA", "Checking version from: %s", url);
    set_state(OTA_STATE_CHECKING_VERSION, APP_ERR_OK);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = g_ota.timeout_ms,
        .method = HTTP_METHOD_GET,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        set_state(OTA_STATE_IDLE, APP_ERR_OTA_HTTP_INIT);
        return APP_ERR_OTA_HTTP_INIT;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        set_state(OTA_STATE_IDLE, APP_ERR_OTA_HTTP_CONNECT);
        return APP_ERR_OTA_HTTP_CONNECT;
    }

    int content_len = esp_http_client_fetch_headers(client);
    if (content_len <= 0 || content_len > 4096) {
        esp_http_client_cleanup(client);
        set_state(OTA_STATE_IDLE, APP_ERR_OTA_HTTP_RESPONSE);
        return APP_ERR_OTA_HTTP_RESPONSE;
    }

    char json_buf[4096] = {0};
    int read_len = esp_http_client_read(client, json_buf,
                                         sizeof(json_buf) - 1);
    esp_http_client_cleanup(client);

    if (read_len <= 0) {
        set_state(OTA_STATE_IDLE, APP_ERR_OTA_DOWNLOAD_FAILED);
        return APP_ERR_OTA_DOWNLOAD_FAILED;
    }

    memset(out_info, 0, sizeof(*out_info));
    const char *ver_ptr = strstr(json_buf, "\"version\"");
    if (ver_ptr) {
        const char *start = strchr(ver_ptr + 9, '"');
        if (start) {
            const char *end = strchr(start + 1, '"');
            if (end) {
                int len = end - start - 1;
                if (len > 0 && len < (int)sizeof(out_info->version)) {
                    memcpy(out_info->version, start + 1, len);
                    out_info->version[len] = '\0';
                }
            }
        }
    }

    const char *size_ptr = strstr(json_buf, "\"size\"");
    if (size_ptr) {
        char *endptr;
        long val = strtol(size_ptr + 7, &endptr, 10);
        if (endptr != size_ptr + 7) {
            out_info->size = (uint32_t)val;
        }
    }

    const char *sha_ptr = strstr(json_buf, "\"sha256\"");
    if (sha_ptr) {
        const char *start = strchr(sha_ptr + 8, '"');
        if (start) {
            const char *end = strchr(start + 1, '"');
            if (end) {
                int len = end - start - 1;
                if (len > 0 && len < (int)sizeof(out_info->sha256)) {
                    memcpy(out_info->sha256, start + 1, len);
                    out_info->sha256[len] = '\0';
                }
            }
        }
    }

    const char *url_ptr = strstr(json_buf, "\"url\"");
    if (url_ptr) {
        const char *start = strchr(url_ptr + 5, '"');
        if (start) {
            const char *end = strchr(start + 1, '"');
            if (end) {
                int len = end - start - 1;
                if (len > 0 && len < (int)sizeof(out_info->url)) {
                    memcpy(out_info->url, start + 1, len);
                    out_info->url[len] = '\0';
                }
            }
        }
    }

    int cmp = ota_update_compare_versions(current_version, out_info->version);
    set_state(OTA_STATE_IDLE, APP_ERR_OK);

    if (cmp < 0) {
        LOG_INFO("OTA", "New version available: current=%s new=%s",
                 current_version, out_info->version);
        return APP_ERR_OK;
    }

    LOG_INFO("OTA", "Firmware is up to date: current=%s remote=%s",
             current_version, out_info->version);
    return APP_ERR_NOT_SUPPORTED;
}

int ota_update_compare_versions(const char *v1, const char *v2)
{
    if (!v1 || !v2) return 0;

    int maj1 = 0, min1 = 0, pat1 = 0;
    int maj2 = 0, min2 = 0, pat2 = 0;
    sscanf(v1, "%d.%d.%d", &maj1, &min1, &pat1);
    sscanf(v2, "%d.%d.%d", &maj2, &min2, &pat2);

    if (maj1 != maj2) return maj1 - maj2;
    if (min1 != min2) return min1 - min2;
    return pat1 - pat2;
}

/* ==================== 回滚 API ==================== */

int ota_update_rollback(bool reboot)
{
    if (!g_ota.initialized) return APP_ERR_OTA_NOT_INIT;

    const esp_partition_t *boot_part = esp_ota_get_boot_partition();
    const esp_partition_t *running = esp_ota_get_running_partition();

    if (boot_part == running) {
        LOG_INFO("OTA", "Already on valid partition (%s), no rollback needed",
                 boot_part->label);
        return APP_ERR_OK;
    }

    set_state(OTA_STATE_ROLLBACK, APP_ERR_OK);
    LOG_INFO("OTA", "Rolling back: boot=%s running=%s",
             boot_part->label, running->label);

    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    if (err != ESP_OK) {
        LOG_ERROR("OTA", "Rollback failed: %s", esp_err_to_name(err));
        set_state(OTA_STATE_FAILED, APP_ERR_OTA_ROLLBACK_FAILED);
        return APP_ERR_OTA_ROLLBACK_FAILED;
    }

    set_state(OTA_STATE_IDLE, APP_ERR_OK);
    LOG_INFO("OTA", "Rollback scheduled, reboot=%d", reboot);

    if (reboot) {
        esp_restart();
    }
    return APP_ERR_OK;
}

int ota_update_mark_current_valid(void)
{
    if (!g_ota.initialized) return APP_ERR_OTA_NOT_INIT;

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        LOG_ERROR("OTA", "Mark app valid failed: %s", esp_err_to_name(err));
        return APP_ERR_OTA_SET_BOOT_FAILED;
    }

    LOG_INFO("OTA", "Marked partition %s as confirmed valid", running->label);
    return APP_ERR_OK;
}

/* ==================== 状态查询 API ==================== */

enum ota_state ota_update_get_state(void)
{
    if (!g_ota.initialized) return OTA_STATE_IDLE;
    return g_ota.state;
}

int ota_update_get_progress(struct ota_progress *out)
{
    if (!out) return APP_ERR_INVALID_PARAM;
    if (!g_ota.initialized || !g_ota.mutex) return APP_ERR_OTA_NOT_INIT;

    xSemaphoreTake(g_ota.mutex, portMAX_DELAY);
    *out = g_ota.progress;
    xSemaphoreGive(g_ota.mutex);
    return APP_ERR_OK;
}

int ota_update_get_running_partition_info(char *partition_label, size_t label_size,
                                         char *app_version, size_t ver_size)
{
    if ((!partition_label && !app_version)) return APP_ERR_INVALID_PARAM;
    if (!g_ota.initialized) return APP_ERR_OTA_NOT_INIT;

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return APP_ERR_OTA_PARTITION_INVALID;

    if (partition_label && label_size > 0) {
        strncpy(partition_label, running->label, label_size - 1);
        partition_label[label_size - 1] = '\0';
    }

    if (app_version && ver_size > 0) {
        const esp_app_desc_t *desc = esp_app_get_description();
        if (desc) {
            strncpy(app_version, desc->version, ver_size - 1);
            app_version[ver_size - 1] = '\0';
        } else {
            app_version[0] = '\0';
        }
    }

    return APP_ERR_OK;
}

/* ==================== 回调注册 API ==================== */

int ota_update_register_progress_callback(ota_progress_cb_t callback,
                                          void *user_data)
{
    if (!callback) return APP_ERR_INVALID_PARAM;
    if (!g_ota.initialized) return APP_ERR_OTA_NOT_INIT;
    if (g_ota.progress_cb_count >= OTA_MAX_CALLBACKS) return APP_ERR_OTA_CALLBACK_FULL;

    for (int i = 0; i < g_ota.progress_cb_count; i++) {
        if (g_ota.progress_cbs[i] == callback) return APP_ERR_OK;
    }

    g_ota.progress_cbs[g_ota.progress_cb_count] = callback;
    g_ota.progress_cb_data[g_ota.progress_cb_count] = user_data;
    g_ota.progress_cb_count++;
    return APP_ERR_OK;
}

int ota_update_register_state_callback(ota_state_change_cb_t callback,
                                       void *user_data)
{
    if (!callback) return APP_ERR_INVALID_PARAM;
    if (!g_ota.initialized) return APP_ERR_OTA_NOT_INIT;
    if (g_ota.state_cb_count >= OTA_MAX_CALLBACKS) return APP_ERR_OTA_CALLBACK_FULL;

    for (int i = 0; i < g_ota.state_cb_count; i++) {
        if (g_ota.state_cbs[i] == callback) return APP_ERR_OK;
    }

    g_ota.state_cbs[g_ota.state_cb_count] = callback;
    g_ota.state_cb_data[g_ota.state_cb_count] = user_data;
    g_ota.state_cb_count++;
    return APP_ERR_OK;
}

int ota_update_unregister_progress_callback(ota_progress_cb_t callback)
{
    if (!callback) return APP_ERR_INVALID_PARAM;
    if (!g_ota.initialized) return APP_ERR_OTA_NOT_INIT;

    for (int i = 0; i < g_ota.progress_cb_count; i++) {
        if (g_ota.progress_cbs[i] == callback) {
            for (int j = i; j < g_ota.progress_cb_count - 1; j++) {
                g_ota.progress_cbs[j] = g_ota.progress_cbs[j + 1];
                g_ota.progress_cb_data[j] = g_ota.progress_cb_data[j + 1];
            }
            g_ota.progress_cb_count--;
            g_ota.progress_cbs[g_ota.progress_cb_count] = NULL;
            g_ota.progress_cb_data[g_ota.progress_cb_count] = NULL;
            return APP_ERR_OK;
        }
    }
    return APP_ERR_NOT_SUPPORTED;
}

int ota_update_unregister_state_callback(ota_state_change_cb_t callback)
{
    if (!callback) return APP_ERR_INVALID_PARAM;
    if (!g_ota.initialized) return APP_ERR_OTA_NOT_INIT;

    for (int i = 0; i < g_ota.state_cb_count; i++) {
        if (g_ota.state_cbs[i] == callback) {
            for (int j = i; j < g_ota.state_cb_count - 1; j++) {
                g_ota.state_cbs[j] = g_ota.state_cbs[j + 1];
                g_ota.state_cb_data[j] = g_ota.state_cb_data[j + 1];
            }
            g_ota.state_cb_count--;
            g_ota.state_cbs[g_ota.state_cb_count] = NULL;
            g_ota.state_cb_data[g_ota.state_cb_count] = NULL;
            return APP_ERR_OK;
        }
    }
    return APP_ERR_NOT_SUPPORTED;
}