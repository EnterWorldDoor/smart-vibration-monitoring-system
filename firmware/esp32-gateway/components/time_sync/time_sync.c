/**
 * @file time_sync.c
 * @author EnterWorldDoor
 * @brief 企业级时间同步实现：SNTP 网络同步 + 本地高精度计时器 + 自动周期同步
 *
 * 架构设计:
 *   - SNTP 模式: 通过 LWIP 内置 sntp 组件获取网络时间
 *   - 本地模式: 基于 esp_timer_get_time() 提供相对时间戳
 *   - 自动周期同步: 后台任务按配置间隔定期触发同步
 *   - 所有共享状态通过 Mutex 保护，确保多任务并发安全
 *   - 与 config_manager 集成，自动读取时间同步配置参数
 */

#include "time_sync.h"
#include "log_system.h"
#include "config_manager.h"
#include "global_error.h"
#include "esp_timer.h"
#include "lwip/apps/sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <time.h>

/* ==================== 模块内部状态 ==================== */

static struct {
    bool initialized;
    bool use_sntp;
    enum sync_status status;
    SemaphoreHandle_t mutex;
    EventGroupHandle_t sync_event;
    TaskHandle_t auto_sync_task;
    TaskHandle_t monitor_task;       /**< SNTP 同步监控任务句柄 */

    /* 时间偏移量 */
    int64_t boot_time_us;             /**< 系统启动时的绝对时间偏移 */
    int64_t last_read_us;              /**< 上次读取的时间戳（用于计算漂移）*/

    /* 配置参数 */
    char timezone[TIME_SYNC_MAX_TIMEZONE_LEN];
    struct sntp_server_config servers[TIME_SYNC_MAX_SERVERS];
    int server_count;
    uint32_t sync_timeout_ms;
    uint32_t retry_interval_ms;
    int max_retries;
    bool auto_sync_enabled;
    uint32_t sync_interval_s;

    /* 回调注册 */
    time_sync_callback_t callbacks[TIME_SYNC_MAX_CALLBACKS];
    void *callback_data[TIME_SYNC_MAX_CALLBACKS];
    int callback_count;
} g_ts = {0};

/* EventGroup 位定义 */
static const int SYNC_COMPLETE_BIT = BIT0;
static const int AUTO_SYNC_STOP_BIT  = BIT1;

/* ==================== 内部辅助函数 ==================== */

/**
 * notify_callbacks - 内部函数：通知所有已注册的同步回调（调用者需持有锁）
 * @status: 同步结果状态
 * @timestamp_us: 同步后的绝对时间戳
 */
static void notify_callbacks(enum sync_status status, int64_t timestamp_us)
{
    for (int i = 0; i < g_ts.callback_count; i++) {
        if (g_ts.callbacks[i]) {
            g_ts.callbacks[i](status, timestamp_us,
                             g_ts.callback_data[i]);
        }
    }
}

/**
 * load_config_from_manager - 从 config_manager 加载时间同步配置
 *
 * Return: APP_ERR_OK or error code
 */
static int load_config_from_manager(void)
{
    if (!config_manager_is_initialized()) {
        LOG_WARN("TIME", "config_manager not initialized, using defaults");
        strncpy(g_ts.timezone, "CST-8", TIME_SYNC_MAX_TIMEZONE_LEN - 1);
        strncpy(g_ts.servers[0].server, "pool.ntp.org",
                TIME_SYNC_MAX_SERVER_LEN - 1);
        strncpy(g_ts.servers[1].server, "time.google.com",
                TIME_SYNC_MAX_SERVER_LEN - 1);
        g_ts.server_count = 2;
        g_ts.sync_timeout_ms = TIME_SYNC_DEFAULT_TIMEOUT_MS;
        g_ts.retry_interval_ms = TIME_SYNC_DEFAULT_RETRY_MS;
        g_ts.max_retries = TIME_SYNC_DEFAULT_MAX_RETRIES;
        g_ts.auto_sync_enabled = true;
        g_ts.sync_interval_s = TIME_SYNC_DEFAULT_INTERVAL_S;
        return APP_ERR_OK;
    }

    const struct system_config *cfg = config_manager_get();
    if (!cfg) return APP_ERR_CONFIG_LOAD_FAIL;

    if (cfg->timezone[0]) {
        strncpy(g_ts.timezone, cfg->timezone, TIME_SYNC_MAX_TIMEZONE_LEN - 1);
    } else {
        strncpy(g_ts.timezone, "CST-8", TIME_SYNC_MAX_TIMEZONE_LEN - 1);
    }

    if (cfg->sntp_server1[0]) {
        strncpy(g_ts.servers[0].server, cfg->sntp_server1,
                TIME_SYNC_MAX_SERVER_LEN - 1);
        g_ts.servers[0].enabled = true;
    }
    if (cfg->sntp_server2[0]) {
        strncpy(g_ts.servers[1].server, cfg->sntp_server2,
                TIME_SYNC_MAX_SERVER_LEN - 1);
        g_ts.servers[1].enabled = true;
    }
    g_ts.server_count = (cfg->sntp_server1[0] ? 1 : 0) +
                       (cfg->sntp_server2[0] ? 1 : 0);
    if (g_ts.server_count == 0) {
        strncpy(g_ts.servers[0].server, "pool.ntp.org",
                TIME_SYNC_MAX_SERVER_LEN - 1);
        g_ts.servers[0].enabled = true;
        g_ts.server_count = 1;
    }

    g_ts.sync_timeout_ms = cfg->sntp_sync_timeout_ms > 0 ?
                           cfg->sntp_sync_timeout_ms : TIME_SYNC_DEFAULT_TIMEOUT_MS;
    g_ts.retry_interval_ms = cfg->sntp_retry_interval_ms > 0 ?
                            cfg->sntp_retry_interval_ms : TIME_SYNC_DEFAULT_RETRY_MS;
    g_ts.max_retries = cfg->sntp_max_retries > 0 ?
                        cfg->sntp_max_retries : TIME_SYNC_DEFAULT_MAX_RETRIES;
    g_ts.auto_sync_enabled = cfg->sntp_auto_sync_enabled;
    g_ts.sync_interval_s = cfg->sntp_sync_interval_s > 0 ?
                          cfg->sntp_sync_interval_s : TIME_SYNC_DEFAULT_INTERVAL_S;

    LOG_INFO("TIME", "Config loaded: tz=%s servers=%d timeout=%u interval=%u",
             g_ts.timezone, g_ts.server_count,
             g_ts.sync_timeout_ms, g_ts.sync_interval_s);
    return APP_ERR_OK;
}

/**
 * apply_timezone - 应用时区设置
 */
static void apply_timezone(const char *tz)
{
    if (tz && tz[0]) {
        setenv("TZ", tz, 1);
        tzset();
        LOG_INFO("TIME", "Timezone set to: %s", tz);
    }
}

/**
 * sync_monitor_task - LWIP SNTP 同步状态监控任务
 *
 * 由于 LWIP SNTP 不支持同步完成回调（不同于 esp_sntp），
 * 通过轮询 sntp_getoperatingmode() 来检测同步完成事件
 */
static void sync_monitor_task(void *arg)
{
    (void)arg;
    uint8_t last_state = SNTP_OPMODE_POLL;
    bool synced_once = false;

    LOG_INFO("TIME", "Sync monitor task started");

    for (;;) {
        if (!g_ts.initialized || !g_ts.use_sntp) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        uint8_t current_state = sntp_getoperatingmode();

        /* 检测从未同步到已同步的状态转换 */
        if (!synced_once &&
            last_state == SNTP_OPMODE_POLL &&
            current_state != SNTP_OPMODE_POLL) {
            xSemaphoreTake(g_ts.mutex, portMAX_DELAY);

            time_t now = time(NULL);
            int64_t now_us = esp_timer_get_time();
            g_ts.boot_time_us = (int64_t)now * 1000000LL - now_us;
            g_ts.last_read_us = g_ts.boot_time_us + now_us;
            g_ts.status = SYNC_STATUS_SYNCED;

            LOG_INFO("TIME", "SNTP synchronized: %s", ctime(&now));

            xEventGroupSetBits(g_ts.sync_event, SYNC_COMPLETE_BIT);
            notify_callbacks(SYNC_STATUS_SYNCED, g_ts.last_read_us);

            xSemaphoreGive(g_ts.mutex);
            synced_once = true;
        }

        /* 持续监控后续的重新同步 */
        if (synced_once &&
            last_state != SNTP_OPMODE_POLL &&
            current_state != last_state &&
            current_state != SNTP_OPMODE_POLL) {
            xSemaphoreTake(g_ts.mutex, portMAX_DELAY);

            time_t now = time(NULL);
            int64_t now_us = esp_timer_get_time();
            g_ts.boot_time_us = (int64_t)now * 1000000LL - now_us;
            g_ts.last_read_us = g_ts.boot_time_us + now_us;

            LOG_INFO("TIME", "SNTP re-synchronized: %s", ctime(&now));

            notify_callbacks(SYNC_STATUS_SYNCED, g_ts.last_read_us);

            xSemaphoreGive(g_ts.mutex);
        }

        last_state = current_state;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/**
 * auto_sync_task_func - 自动周期性同步后台任务
 */
static void auto_sync_task_func(void *arg)
{
    (void)arg;

    LOG_INFO("TIME", "Auto-sync task started (interval=%u s)",
             g_ts.sync_interval_s);

    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(
            g_ts.sync_event,
            AUTO_SYNC_STOP_BIT,
            pdFALSE,
            pdTRUE,
            pdMS_TO_TICKS(g_ts.sync_interval_s * 1000));

        if (bits & AUTO_SYNC_STOP_BIT) break;

        LOG_INFO("TIME", "Triggering periodic SNTP sync...");
        sntp_stop();
        sntp_init();
    }

    LOG_INFO("TIME", "Auto-sync task stopped");
    vTaskDelete(NULL);
}

/* ==================== 生命周期 API ==================== */

int time_sync_init(bool use_sntp, const char *timezone)
{
    if (g_ts.initialized) return APP_ERR_TIME_ALREADY_INIT;

    memset(&g_ts, 0, sizeof(g_ts));

    g_ts.mutex = xSemaphoreCreateMutex();
    if (!g_ts.mutex) {
        LOG_ERROR("TIME", "Failed to create mutex");
        return APP_ERR_NO_MEM;
    }

    g_ts.use_sntp = use_sntp;
    g_ts.status = SYNC_STATUS_INITIALIZED;

    int ret = load_config_from_manager();
    if (ret != APP_ERR_OK) {
        LOG_WARN("TIME", "Using default config (error: %d)", ret);
    }

    if (timezone && timezone[0]) {
        apply_timezone(timezone);
    } else {
        apply_timezone(g_ts.timezone);
    }

    if (use_sntp) {
        g_ts.sync_event = xEventGroupCreate();
        if (!g_ts.sync_event) {
            vSemaphoreDelete(g_ts.mutex);
            g_ts.mutex = NULL;
            return APP_ERR_TIME_EVENT_GROUP_FAIL;
        }

        sntp_setoperatingmode(SNTP_OPMODE_POLL);
        for (int i = 0; i < g_ts.server_count && i < TIME_SYNC_MAX_SERVERS; i++) {
            if (g_ts.servers[i].enabled) {
                sntp_setservername(i, g_ts.servers[i].server);
                LOG_INFO("TIME", "NTP server[%d]: %s",
                         i, g_ts.servers[i].server);
            }
        }
        sntp_init();

        LOG_INFO("TIME", "SNTP initialized with %d servers, timeout=%u ms",
                 g_ts.server_count, g_ts.sync_timeout_ms);
    } else {
        LOG_INFO("TIME", "Local timer mode only (no SNTP)");
    }

    if (use_sntp && g_ts.auto_sync_enabled) {
        BaseType_t tret = xTaskCreate(
            auto_sync_task_func, "time_auto",
            TIME_SYNC_TASK_STACK_SIZE, NULL,
            TIME_SYNC_TASK_PRIORITY, &g_ts.auto_sync_task);
        if (tret != pdPASS) {
            LOG_WARN("TIME", "Failed to create auto-sync task");
        }
    }

    /* 启动 SNTP 同步状态监控任务（LWIP 模式必须）*/
    if (use_sntp) {
        BaseType_t ret = xTaskCreate(
            sync_monitor_task, "sntp_mon",
            TIME_SYNC_MONITOR_STACK_SIZE,
            NULL,
            TIME_SYNC_TASK_PRIORITY + 1,
            &g_ts.monitor_task);
        if (ret != pdPASS) {
            LOG_WARN("TIME", "Failed to create sync monitor task");
            g_ts.monitor_task = NULL;
        } else {
            LOG_DEBUG("TIME", "Sync monitor task created successfully");
        }
    }

    g_ts.initialized = true;
    LOG_INFO("TIME", "Module initialized (mode=%s, tz=%s)",
             use_sntp ? "SNTP" : "LOCAL", g_ts.timezone);
    return APP_ERR_OK;
}

int time_sync_deinit(void)
{
    if (!g_ts.initialized) return APP_ERR_TIME_NOT_INIT;

    xSemaphoreTake(g_ts.mutex, portMAX_DELAY);

    if (g_ts.auto_sync_task) {
        xEventGroupSetBits(g_ts.sync_event, AUTO_SYNC_STOP_BIT);
        xSemaphoreGive(g_ts.mutex);
        vTaskDelay(pdMS_TO_TICKS(100));
        xSemaphoreTake(g_ts.mutex, portMAX_DELAY);
        g_ts.auto_sync_task = NULL;
    }

    if (g_ts.use_sntp) {
        sntp_stop();
    }

    if (g_ts.sync_event) {
        vEventGroupDelete(g_ts.sync_event);
        g_ts.sync_event = NULL;
    }

    /* 删除 SNTP 同步监控任务 */
    if (g_ts.monitor_task) {
        vTaskDelete(g_ts.monitor_task);
        g_ts.monitor_task = NULL;
    }

    g_ts.initialized = false;
    g_ts.callback_count = 0;
    memset(g_ts.callbacks, 0, sizeof(g_ts.callbacks));
    memset(g_ts.callback_data, 0, sizeof(g_ts.callback_data));

    xSemaphoreGive(g_ts.mutex);
    vSemaphoreDelete(g_ts.mutex);
    g_ts.mutex = NULL;

    LOG_INFO("TIME", "Deinitialized");
    return APP_ERR_OK;
}

bool time_sync_is_initialized(void)
{
    return g_ts.initialized;
}

/* ==================== 时间戳获取 API ==================== */

int64_t time_sync_get_timestamp_us(void)
{
    if (!g_ts.initialized) return esp_timer_get_time();

    int64_t now_us = esp_timer_get_time();

    if (g_ts.use_sntp && g_ts.boot_time_us != 0) {
        xSemaphoreTake(g_ts.mutex, portMAX_DELAY);
        int64_t abs_time = g_ts.boot_time_us + now_us;
        g_ts.last_read_us = abs_time;
        xSemaphoreGive(g_ts.mutex);
        return abs_time;
    }

    return now_us;
}

int time_sync_get_time_info(struct time_info *info)
{
    if (!info) return APP_ERR_INVALID_PARAM;
    if (!g_ts.initialized || !g_ts.mutex) return APP_ERR_TIME_NOT_INIT;

    xSemaphoreTake(g_ts.mutex, portMAX_DELAY);

    memset(info, 0, sizeof(*info));

    int64_t now_us = esp_timer_get_time();
    info->uptime_s = (uint32_t)(now_us / 1000000ULL);

    if (g_ts.use_sntp && g_ts.boot_time_us != 0) {
        info->timestamp_us = g_ts.boot_time_us + now_us;
        info->is_synchronized = true;
        info->last_sync_epoch = (uint32_t)(info->timestamp_us / 1000000LL);
        info->drift_us = (int32_t)(info->timestamp_us - g_ts.last_read_us);
        g_ts.last_read_us = info->timestamp_us;
    } else {
        info->timestamp_us = now_us;
        info->is_synchronized = false;
    }

    info->timestamp_ms = info->timestamp_us / 1000;
    info->status = g_ts.status;

    time_t epoch_sec = (time_t)(info->timestamp_us / 1000000LL);
    localtime_r(&epoch_sec, &info->local_time);
    gmtime_r(&epoch_sec, &info->utc_time);

    xSemaphoreGive(g_ts.mutex);
    return APP_ERR_OK;
}

/* ==================== 同步控制 API ==================== */

int time_sync_wait_sync(int timeout_ms)
{
    if (!g_ts.initialized) return APP_ERR_TIME_NOT_INIT;
    if (!g_ts.use_sntp) {
        LOG_WARN("TIME", "SNTP disabled, returning immediately");
        return APP_ERR_OK;
    }
    if (!g_ts.sync_event) return APP_ERR_TIME_SNTPI_INIT_FAIL;

    TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(
        g_ts.sync_event, SYNC_COMPLETE_BIT,
        pdFALSE, pdTRUE, ticks);

    if (bits & SYNC_COMPLETE_BIT) return APP_ERR_OK;
    return APP_ERR_TIME_SNTPI_SYNC_TIMEOUT;
}

int time_sync_force_sync(void)
{
    if (!g_ts.initialized) return APP_ERR_TIME_NOT_INIT;
    if (!g_ts.use_sntp) return APP_ERR_TIME_SNTPI_INIT_FAIL;

    xSemaphoreTake(g_ts.mutex, portMAX_DELAY);
    g_ts.status = SYNC_STATUS_SYNCHRONIZING;
    xSemaphoreGive(g_ts.mutex);

    LOG_INFO("TIME", "Force sync triggered");
    sntp_stop();
    sntp_init();
    return APP_ERR_OK;
}

enum sync_status time_sync_get_status(void)
{
    if (!g_ts.initialized) return SYNC_STATUS_IDLE;
    return g_ts.status;
}

bool time_sync_is_synchronized(void)
{
    if (!g_ts.initialized) return false;
    return (g_ts.boot_time_us != 0);
}

/* ==================== NTP 服务器管理 API ==================== */

int time_sync_set_servers(const struct sntp_server_config *servers, int count)
{
    if (!servers || count <= 0 || count > TIME_SYNC_MAX_SERVERS) {
        return APP_ERR_INVALID_PARAM;
    }
    if (!g_ts.initialized) return APP_ERR_TIME_NOT_INIT;

    xSemaphoreTake(g_ts.mutex, portMAX_DELAY);

    memcpy(g_ts.servers, servers, count * sizeof(*servers));
    g_ts.server_count = count;

    if (g_ts.use_sntp) {
        for (int i = 0; i < count && i < TIME_SYNC_MAX_SERVERS; i++) {
            if (servers[i].enabled) {
                sntp_setservername(i, servers[i].server);
            }
        }
    }

    xSemaphoreGive(g_ts.mutex);
    LOG_INFO("TIME", "Servers updated: %d configured", count);
    return APP_ERR_OK;
}

int time_sync_get_servers(struct sntp_server_config *servers,
                         int max_count, int *out_count)
{
    if (!servers || max_count <= 0 || !out_count) return APP_ERR_INVALID_PARAM;
    if (!g_ts.initialized) return APP_ERR_TIME_NOT_INIT;

    xSemaphoreTake(g_ts.mutex, portMAX_DELAY);

    int copy_count = (g_ts.server_count < max_count) ? g_ts.server_count : max_count;
    memcpy(servers, g_ts.servers, copy_count * sizeof(*servers));
    *out_count = copy_count;

    xSemaphoreGive(g_ts.mutex);
    return APP_ERR_OK;
}

/* ==================== 回调注册 API ==================== */

int time_sync_register_callback(time_sync_callback_t callback, void *user_data)
{
    if (!callback) return APP_ERR_INVALID_PARAM;
    if (!g_ts.initialized) return APP_ERR_TIME_NOT_INIT;
    if (g_ts.callback_count >= TIME_SYNC_MAX_CALLBACKS) {
        return APP_ERR_TIME_CALLBACK_FULL;
    }

    for (int i = 0; i < g_ts.callback_count; i++) {
        if (g_ts.callbacks[i] == callback) return APP_ERR_OK;
    }

    g_ts.callbacks[g_ts.callback_count] = callback;
    g_ts.callback_data[g_ts.callback_count] = user_data;
    g_ts.callback_count++;

    LOG_DEBUG("TIME", "Callback registered (total=%d)", g_ts.callback_count);
    return APP_ERR_OK;
}

int time_sync_unregister_callback(time_sync_callback_t callback)
{
    if (!callback) return APP_ERR_INVALID_PARAM;
    if (!g_ts.initialized) return APP_ERR_TIME_NOT_INIT;

    for (int i = 0; i < g_ts.callback_count; i++) {
        if (g_ts.callbacks[i] == callback) {
            for (int j = i; j < g_ts.callback_count - 1; j++) {
                g_ts.callbacks[j] = g_ts.callbacks[j + 1];
                g_ts.callback_data[j] = g_ts.callback_data[j + 1];
            }
            g_ts.callback_count--;
            g_ts.callbacks[g_ts.callback_count] = NULL;
            g_ts.callback_data[g_ts.callback_count] = NULL;
            LOG_DEBUG("TIME", "Callback unregistered (remaining=%d)",
                     g_ts.callback_count);
            return APP_ERR_OK;
        }
    }
    return APP_ERR_NOT_SUPPORTED;
}