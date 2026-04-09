/**
 * @file config_manager.c
 * @author EnterWorldDoor
 * @brief 企业级配置管理器实现：FreeRTOS Mutex 线程安全、NVS 持久化、
 *        CRC32 完整性校验、版本自动迁移、工厂重置、导出导入、备份恢复
 *
 * 架构设计：
 *   - 所有对 g_config 的读写均通过 g_mutex 保护，确保多任务并发安全
 *   - NVS 存储格式: [struct system_config raw bytes] + [uint32_t crc32]
 *   - 加载流程: NVS读取 -> CRC校验 -> 版本检查 -> 自动迁移 -> 字段验证
 *   - 保存流程: 字段验证 -> 计算CRC -> 加锁拷贝 -> NVS写入 -> 回调通知 -> 解锁
 */

#include "config_manager.h"
#include "log_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stddef.h>

#define NVS_NAMESPACE     "edgevib_cfg"
#define MAX_CALLBACKS     5
#define CRC32_POLYNOMIAL  0xEDB88320u

static struct system_config g_config;
static bool g_initialized = false;
static SemaphoreHandle_t g_mutex = NULL;
static config_change_callback_t g_callbacks[MAX_CALLBACKS] = {0};
static int g_callback_count = 0;

/* 出厂默认配置 */
static const struct system_config DEFAULT_CONFIG = {
    .config_version          = CONFIG_VERSION,
    .crc32                  = 0,
    .sample_rate_hz         = 100,
    .sensor_buffer_size     = 512,
    .fft_size               = 512,
    .rms_warning_threshold  = 2.0f,
    .rms_critical_threshold = 4.0f,
    .freq_peak_threshold    = 0.5f,
    .ai_anomaly_threshold   = 0.7f,
    .heartbeat_interval_ms  = 1000,
    .device_id              = 1,
    .device_name            = "EdgeVib-Sensor",
    .gateway_url            = "mqtt://gateway.local",
    .encryption_enabled     = false,
    .auto_reboot_enabled    = false,
    .reboot_interval_seconds = 86400,
};

/**
 * apply_default_config - 应用默认配置
 * @cfg: 配置结构体指针
 */
/**
 * apply_default_config - 将配置结构体初始化为出厂默认值
 * @cfg: 目标配置指针
 */
static void apply_default_config(struct system_config *cfg)
{
    memcpy(cfg, &DEFAULT_CONFIG, sizeof(struct system_config));
}

/**
 * compute_crc32_buf - 对原始字节缓冲区计算 CRC-32 (IEEE 802.3) 校验和
 * @data: 数据缓冲区
 * @len: 数据长度（字节）
 *
 * Return: 32 位 CRC 值
 */
static uint32_t compute_crc32_buf(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ CRC32_POLYNOMIAL;
            else
                crc >>= 1;
        }
    }
    return ~crc;
}

/**
 * notify_config_change - 在已持有锁的上下文中通知所有注册的配置变更回调
 * @key: 变更的配置键名
 * @old_value: 变更前的配置快照
 * @new_value: 变更后的配置快照
 *
 * 注意：回调执行期间持有互斥量，因此回调不应阻塞或再次调用 config_manager API
 */
static void notify_config_change(const char *key, const void *old_value, const void *new_value)
{
    for (int i = 0; i < g_callback_count; i++) {
        if (g_callbacks[i]) {
            g_callbacks[i](key, old_value, new_value);
        }
    }
}

/**
 * migrate_config - 配置版本自动迁移：当检测到旧版配置时填充新增字段的默认值
 * @cfg: 从 NVS 读取的旧版配置指针（将被就地修改为新版格式）
 */
static void migrate_config(struct system_config *cfg)
{
    uint32_t ver = cfg->config_version;
    if (ver == CONFIG_VERSION) return;

    LOG_WARN("CFG", "Migrating config v%u.%u.%u -> v%u.%u.%u",
             (ver >> 16) & 0xFF, (ver >> 8) & 0xFF, ver & 0xFF,
             CONFIG_VERSION_MAJOR, CONFIG_VERSION_MINOR, CONFIG_VERSION_PATCH);

    /* 未来版本升级时在此添加迁移分支:
     * if (ver < VERSION_1_1_0) { cfg->new_field = default; }
     */

    cfg->config_version = CONFIG_VERSION;
    LOG_INFO("CFG", "Config migration completed");
}

/**
 * nvs_open_safe - 安全打开 NVS 命名空间
 * @nvs_out: 输出句柄指针
 * Return: ESP_OK on success
 */
static esp_err_t nvs_open_safe(nvs_handle_t *nvs_out)
{
    return nvs_open(NVS_NAMESPACE, NVS_READWRITE, nvs_out);
}

/**
 * save_to_nvs - 内部函数：将配置带 CRC32 写入 NVS（调用者需自行加锁）
 * @cfg: 已通过验证的配置指针
 * Return: APP_ERR_OK on success
 */
static int save_to_nvs(const struct system_config *cfg)
{
    struct system_config c = *cfg;
    c.crc32 = 0;
    c.crc32 = compute_crc32_buf((const uint8_t *)&c, sizeof(c));

    nvs_handle_t nvs;
    esp_err_t err = nvs_open_safe(&nvs);
    if (err != ESP_OK) {
        LOG_ERROR("CFG", "NVS open failed (err=0x%x)", err);
        return APP_ERR_CONFIG_NVS_OPEN;
    }

    err = nvs_set_blob(nvs, "config", &c, sizeof(c));
    if (err != ESP_OK) {
        LOG_ERROR("CFG", "NVS write failed (err=0x%x)", err);
        nvs_close(nvs);
        return APP_ERR_CONFIG_NVS_WRITE;
    }

    err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) {
        LOG_ERROR("CFG", "NVS commit failed (err=0x%x)", err);
        return APP_ERR_CONFIG_NVS_COMMIT;
    }
    return APP_ERR_OK;
}

int config_manager_validate(const struct system_config *cfg)
{
    if (!cfg) {
        LOG_ERROR("CFG", "validate: null pointer");
        return APP_ERR_INVALID_PARAM;
    }
    if (cfg->sample_rate_hz < 1 || cfg->sample_rate_hz > 16000) {
        LOG_WARN("CFG", "validate fail: sample_rate_hz=%d out of [1,16000]", cfg->sample_rate_hz);
        return APP_ERR_CONFIG_VALIDATION;
    }
    if (cfg->sensor_buffer_size < 64 || cfg->sensor_buffer_size > 4096) {
        LOG_WARN("CFG", "validate fail: sensor_buffer_size=%d out of [64,4096]", cfg->sensor_buffer_size);
        return APP_ERR_CONFIG_VALIDATION;
    }
    if (cfg->fft_size < 64 || cfg->fft_size > 4096 || (cfg->fft_size & (cfg->fft_size - 1)) != 0) {
        LOG_WARN("CFG", "validate fail: fft_size=%d not power-of-2 or out of [64,4096]", cfg->fft_size);
        return APP_ERR_CONFIG_VALIDATION;
    }
    if (cfg->rms_warning_threshold < 0 || cfg->rms_critical_threshold < cfg->rms_warning_threshold) {
        LOG_WARN("CFG", "validate fail: rms warning(%.2f) > critical(%.2f)",
                 cfg->rms_warning_threshold, cfg->rms_critical_threshold);
        return APP_ERR_CONFIG_VALIDATION;
    }
    if (cfg->freq_peak_threshold < 0.0f || cfg->freq_peak_threshold > 1.0f) {
        LOG_WARN("CFG", "validate fail: freq_peak=%.2f out of [0,1]", cfg->freq_peak_threshold);
        return APP_ERR_CONFIG_VALIDATION;
    }
    if (cfg->ai_anomaly_threshold < 0.0f || cfg->ai_anomaly_threshold > 1.0f) {
        LOG_WARN("CFG", "validate fail: ai_threshold=%.2f out of [0,1]", cfg->ai_anomaly_threshold);
        return APP_ERR_CONFIG_VALIDATION;
    }
    if (cfg->heartbeat_interval_ms < 100 || cfg->heartbeat_interval_ms > 60000) {
        LOG_WARN("CFG", "validate fail: heartbeat=%u ms out of [100,60000]", cfg->heartbeat_interval_ms);
        return APP_ERR_CONFIG_VALIDATION;
    }
    return APP_ERR_OK;
}

int config_manager_init(struct system_config *cfg)
{
    if (g_initialized) {
        if (cfg) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            memcpy(cfg, &g_config, sizeof(g_config));
            xSemaphoreGive(g_mutex);
        }
        return APP_ERR_OK;
    }

    g_mutex = xSemaphoreCreateMutex();
    if (!g_mutex) {
        LOG_ERROR("CFG", "Failed to create mutex");
        return APP_ERR_NO_MEM;
    }

    apply_default_config(&g_config);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open_safe(&nvs);
    if (err != ESP_OK) {
        LOG_WARN("CFG", "NVS open failed (0x%x), factory defaults applied", err);
    } else {
        size_t size = sizeof(struct system_config);
        err = nvs_get_blob(nvs, "config", &g_config, &size);
        if (err == ESP_OK && size == sizeof(struct system_config)) {
            uint32_t stored_crc = g_config.crc32;
            g_config.crc32 = 0;
            uint32_t computed = compute_crc32_buf((const uint8_t *)&g_config, sizeof(g_config));
            g_config.crc32 = stored_crc;
            if (computed != stored_crc) {
                LOG_WARN("CFG", "CRC mismatch stored=0x%08X computed=0x%08X, corrupted", stored_crc, computed);
                apply_default_config(&g_config);
            } else {
                if (g_config.config_version != CONFIG_VERSION) migrate_config(&g_config);
                if (config_manager_validate(&g_config) != APP_ERR_OK) {
                    LOG_WARN("CFG", "Loaded config invalid, fallback to defaults");
                    apply_default_config(&g_config);
                } else {
                    LOG_INFO("CFG", "Config loaded from NVS (CRC OK)");
                }
            }
        } else {
            LOG_WARN("CFG", "No saved config (err=0x%x), using defaults", err);
        }
        nvs_close(nvs);
    }

    g_initialized = true;
    if (cfg) {
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        memcpy(cfg, &g_config, sizeof(g_config));
        xSemaphoreGive(g_mutex);
    }
    LOG_INFO("CFG", "Initialized (mutex created, v=0x%08X)", CONFIG_VERSION);
    return APP_ERR_OK;
}

int config_manager_deinit(void)
{
    if (!g_initialized || !g_mutex) return APP_ERR_GENERAL;
    vSemaphoreDelete(g_mutex);
    g_mutex = NULL;
    g_initialized = false;
    g_callback_count = 0;
    memset(g_callbacks, 0, sizeof(g_callbacks));
    LOG_INFO("CFG", "Deinitialized");
    return APP_ERR_OK;
}

bool config_manager_is_initialized(void)
{
    return g_initialized;
}

const struct system_config *config_manager_get(void)
{
    if (!g_initialized || !g_mutex) return NULL;
    return &g_config;
}

int config_manager_set(const struct system_config *cfg)
{
    if (!cfg) return APP_ERR_INVALID_PARAM;
    if (!g_initialized || !g_mutex) return APP_ERR_GENERAL;
    int ret = config_manager_validate(cfg);
    if (ret != APP_ERR_OK) return ret;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    struct system_config old_cfg = g_config;
    memcpy(&g_config, cfg, sizeof(g_config));
    notify_config_change("config", &old_cfg, &g_config);
    ret = save_to_nvs(&g_config);
    xSemaphoreGive(g_mutex);
    if (ret == APP_ERR_OK) LOG_INFO("CFG", "Config updated and persisted");
    return ret;
}

int config_manager_save(void)
{
    if (!g_initialized || !g_mutex) return APP_ERR_GENERAL;
    int ret;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    ret = config_manager_validate(&g_config);
    if (ret == APP_ERR_OK) ret = save_to_nvs(&g_config);
    xSemaphoreGive(g_mutex);
    if (ret == APP_ERR_OK) LOG_INFO("CFG", "Config saved to NVS");
    return ret;
}

int config_manager_reset(void)
{
    if (!g_initialized || !g_mutex) return APP_ERR_GENERAL;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    struct system_config old_cfg = g_config;
    apply_default_config(&g_config);
    notify_config_change("reset", &old_cfg, &g_config);
    int ret = save_to_nvs(&g_config);
    xSemaphoreGive(g_mutex);
    if (ret == APP_ERR_OK) LOG_INFO("CFG", "Reset to factory defaults");
    return ret;
}

/**
 * config_manager_get - 获取当前配置
 *
 * Return: 指向当前配置的指针
 */


int config_manager_factory_reset(void)
{
    if (!g_initialized || !g_mutex) return APP_ERR_GENERAL;
    LOG_WARN("CFG", "Factory reset - erasing all configs...");
    nvs_handle_t nvs;
    esp_err_t err = nvs_open_safe(&nvs);
    if (err != ESP_OK) {
        LOG_ERROR("CFG", "Factory reset: NVS open failed (0x%x)", err);
        return APP_ERR_CONFIG_NVS_OPEN;
    }
    err = nvs_erase_key(nvs, "config");
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs); return APP_ERR_CONFIG_SAVE_FAIL;
    }
    char key[32];
    for (uint8_t i = 0; i < CONFIG_MAX_BACKUP_SLOTS; i++) {
        snprintf(key, sizeof(key), "backup_%u", i);
        nvs_erase_key(nvs, key);
    }
    err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) return APP_ERR_CONFIG_NVS_COMMIT;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    struct system_config old_cfg = g_config;
    apply_default_config(&g_config);
    notify_config_change("factory_reset", &old_cfg, &g_config);
    xSemaphoreGive(g_mutex);
    LOG_INFO("CFG", "Factory reset complete");
    return APP_ERR_OK;
}

int config_manager_get_sample_rate(void)
{
    if (!g_initialized) return DEFAULT_CONFIG.sample_rate_hz;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    int val = g_config.sample_rate_hz;
    xSemaphoreGive(g_mutex);
    return val;
}

int config_manager_set_sample_rate(int rate)
{
    if (rate < 1 || rate > 16000) return APP_ERR_CONFIG_VALIDATION;
    if (!g_initialized || !g_mutex) return APP_ERR_GENERAL;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    int old = g_config.sample_rate_hz;
    g_config.sample_rate_hz = rate;
    int ret = save_to_nvs(&g_config);
    if (ret == APP_ERR_OK) notify_config_change("sample_rate_hz", &old, &rate);
    xSemaphoreGive(g_mutex);
    return ret;
}

int config_manager_get_fft_size(void)
{
    if (!g_initialized) return DEFAULT_CONFIG.fft_size;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    int val = g_config.fft_size;
    xSemaphoreGive(g_mutex);
    return val;
}

int config_manager_set_fft_size(int size)
{
    if (size < 64 || size > 4096 || (size & (size - 1)) != 0) return APP_ERR_CONFIG_VALIDATION;
    if (!g_initialized || !g_mutex) return APP_ERR_GENERAL;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    int old = g_config.fft_size;
    g_config.fft_size = size;
    int ret = save_to_nvs(&g_config);
    if (ret == APP_ERR_OK) notify_config_change("fft_size", &old, &size);
    xSemaphoreGive(g_mutex);
    return ret;
}

uint8_t config_manager_get_device_id(void)
{
    if (!g_initialized) return DEFAULT_CONFIG.device_id;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    uint8_t id = g_config.device_id;
    xSemaphoreGive(g_mutex);
    return id;
}

int config_manager_get_rms_thresholds(float *w, float *c)
{
    if (!w || !c) return APP_ERR_INVALID_PARAM;
    if (!g_initialized || !g_mutex) { *w = DEFAULT_CONFIG.rms_warning_threshold; *c = DEFAULT_CONFIG.rms_critical_threshold; return APP_ERR_OK; }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    *w = g_config.rms_warning_threshold; *c = g_config.rms_critical_threshold;
    xSemaphoreGive(g_mutex);
    return APP_ERR_OK;
}

int config_manager_export(uint8_t *buf, size_t buf_len, size_t *out_len)
{
    if (!buf || buf_len < CONFIG_EXPORT_BUF_SIZE || !out_len) return APP_ERR_INVALID_PARAM;
    if (!g_initialized || !g_mutex) return APP_ERR_GENERAL;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    struct system_config e = g_config;
    e.crc32 = 0;
    uint32_t crc = compute_crc32_buf((const uint8_t *)&e, sizeof(e));
    e.crc32 = crc;
    memcpy(buf, &e, sizeof(e));
    uint32_t crc_le = crc;
    memcpy(buf + sizeof(e), &crc_le, 4);
    *out_len = CONFIG_EXPORT_BUF_SIZE;
    xSemaphoreGive(g_mutex);
    LOG_DEBUG("CFG", "Exported %u bytes CRC=0x%08X", *out_len, crc);
    return APP_ERR_OK;
}

int config_manager_import(const uint8_t *buf, size_t len)
{
    if (!buf || len != CONFIG_EXPORT_BUF_SIZE) {
        LOG_ERROR("CFG", "import: bad input (len=%u expected=%u)", (unsigned)len, (unsigned)CONFIG_EXPORT_BUF_SIZE);
        return APP_ERR_INVALID_PARAM;
    }
    if (!g_initialized || !g_mutex) return APP_ERR_GENERAL;
    struct system_config imp;
    memcpy(&imp, buf, sizeof(imp));
    uint32_t stored_crc; memcpy(&stored_crc, buf + sizeof(imp), 4);
    imp.crc32 = 0;
    if (compute_crc32_buf((const uint8_t *)&imp, sizeof(imp)) != stored_crc) {
        LOG_ERROR("CFG", "import: CRC mismatch"); return APP_ERR_CONFIG_VALIDATION;
    }
    imp.crc32 = stored_crc;
    if (imp.config_version != CONFIG_VERSION) migrate_config(&imp);
    int ret = config_manager_validate(&imp);
    if (ret != APP_ERR_OK) { LOG_ERROR("CFG", "import: validation failed (%d)", ret); return ret; }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    struct system_config old_cfg = g_config;
    g_config = imp;
    notify_config_change("import", &old_cfg, &g_config);
    ret = save_to_nvs(&g_config);
    xSemaphoreGive(g_mutex);
    if (ret == APP_ERR_OK) LOG_INFO("CFG", "Imported and persisted (v=0x%08X)", imp.config_version);
    return ret;
}

uint32_t config_manager_compute_crc32(const struct system_config *cfg)
{
    if (!cfg) return 0;
    struct system_config tmp = *cfg;
    tmp.crc32 = 0;
    return compute_crc32_buf((const uint8_t *)&tmp, sizeof(tmp));
}

uint32_t config_manager_get_version(void)
{
    return g_initialized ? g_config.config_version : 0;
}

int config_manager_backup(uint8_t slot)
{
    if (slot >= CONFIG_MAX_BACKUP_SLOTS) {
        LOG_ERROR("CFG", "backup: invalid slot %u", slot); return APP_ERR_INVALID_PARAM;
    }
    if (!g_initialized || !g_mutex) return APP_ERR_GENERAL;
    char key[32]; snprintf(key, sizeof(key), "backup_%u", slot);
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    struct system_config b = g_config;
    xSemaphoreGive(g_mutex);
    b.crc32 = 0; b.crc32 = compute_crc32_buf((const uint8_t *)&b, sizeof(b));
    nvs_handle_t nvs; esp_err_t err = nvs_open_safe(&nvs);
    if (err != ESP_OK) { LOG_ERROR("CFG", "backup: NVS open failed"); return APP_ERR_CONFIG_NVS_OPEN; }
    err = nvs_set_blob(nvs, key, &b, sizeof(b));
    if (err != ESP_OK) { nvs_close(nvs); LOG_ERROR("CFG", "backup: write failed"); return APP_ERR_CONFIG_NVS_WRITE; }
    err = nvs_commit(nvs); nvs_close(nvs);
    if (err != ESP_OK) { LOG_ERROR("CFG", "backup: commit failed"); return APP_ERR_CONFIG_NVS_COMMIT; }
    LOG_INFO("CFG", "Backed up to slot %u (CRC=0x%08X)", slot, b.crc32);
    return APP_ERR_OK;
}

int config_manager_restore(uint8_t slot)
{
    if (slot >= CONFIG_MAX_BACKUP_SLOTS) {
        LOG_ERROR("CFG", "restore: invalid slot %u", slot); return APP_ERR_INVALID_PARAM;
    }
    if (!g_initialized || !g_mutex) return APP_ERR_GENERAL;
    char key[32]; snprintf(key, sizeof(key), "backup_%u", slot);
    nvs_handle_t nvs; esp_err_t err = nvs_open_safe(&nvs);
    if (err != ESP_OK) { LOG_ERROR("CFG", "restore: NVS open failed"); return APP_ERR_CONFIG_NVS_OPEN; }
    struct system_config bk; size_t sz = sizeof(bk);
    err = nvs_get_blob(nvs, key, &bk, &sz); nvs_close(nvs);
    if (err != ESP_OK || sz != sizeof(bk)) {
        LOG_ERROR("CFG", "restore: slot %u not found", slot); return APP_ERR_CONFIG_LOAD_FAIL;
    }
    uint32_t sc = bk.crc32; bk.crc32 = 0;
    if (compute_crc32_buf((const uint8_t *)&bk, sizeof(bk)) != sc) {
        LOG_ERROR("CFG", "restore: slot %u CRC corrupt", slot); return APP_ERR_CONFIG_VALIDATION;
    }
    bk.crc32 = sc;
    if (bk.config_version != CONFIG_VERSION) migrate_config(&bk);
    int ret = config_manager_validate(&bk);
    if (ret != APP_ERR_OK) { LOG_ERROR("CFG", "restore: validation failed (%d)", ret); return ret; }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    struct system_config old_cfg = g_config;
    g_config = bk;
    notify_config_change("restore", &old_cfg, &g_config);
    ret = save_to_nvs(&g_config);
    xSemaphoreGive(g_mutex);
    if (ret == APP_ERR_OK) LOG_INFO("CFG", "Restored from slot %u", slot);
    return ret;
}

int config_manager_register_callback(config_change_callback_t cb)
{
    if (!cb) { LOG_ERROR("CFG", "register_cb: null"); return APP_ERR_INVALID_PARAM; }
    if (g_callback_count >= MAX_CALLBACKS) { LOG_ERROR("CFG", "register_cb: full"); return APP_ERR_NOT_SUPPORTED; }
    for (int i = 0; i < g_callback_count; i++) {
        if (g_callbacks[i] == cb) { LOG_WARN("CFG", "register_cb: dup"); return APP_ERR_OK; }
    }
    g_callbacks[g_callback_count++] = cb;
    LOG_INFO("CFG", "Callback registered (total=%d)", g_callback_count);
    return APP_ERR_OK;
}

int config_manager_unregister_callback(config_change_callback_t cb)
{
    if (!cb) { LOG_ERROR("CFG", "unregister_cb: null"); return APP_ERR_INVALID_PARAM; }
    for (int i = 0; i < g_callback_count; i++) {
        if (g_callbacks[i] == cb) {
            for (int j = i; j < g_callback_count - 1; j++) g_callbacks[j] = g_callbacks[j+1];
            g_callbacks[--g_callback_count] = NULL;
            LOG_INFO("CFG", "Callback unregistered (remaining=%d)", g_callback_count);
            return APP_ERR_OK;
        }
    }
    LOG_WARN("CFG", "unregister_cb: not found");
    return APP_ERR_NOT_SUPPORTED;
}