/**
 * @file data_manager.c
 * @author EnterWorldDoor
 * @brief 企业级数据管理器实现: 环形缓冲、通知队列、订阅分发、统计
 *
 * Thread safety: FreeRTOS mutex protects ring buffer. Callbacks dispatched
 * outside mutex to prevent deadlock in subscriber implementations.
 *
 * Notification: internal FreeRTOS queue (depth 4) signals waiting consumers.
 * Each push() sends one placeholder; consumers call wait_for_new() to block.
 */

#include "data_manager.h"
#include "global_error.h"
#include "log_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG = "DATA-MGR";

/* ==================== Internal state (static allocation) ==================== */

static system_data_packet_t s_ringbuf[DATA_MANAGER_HISTORY];
static uint32_t s_write_idx   = 0;    /* Next write position (circular) */
static uint32_t s_sequence    = 0;    /* Monotonically increasing packet seq */
static uint32_t s_stored_count = 0;   /* How many packets have been written */

static SemaphoreHandle_t s_mutex = NULL;
static QueueHandle_t    s_notify_q = NULL;

static data_manager_cb_t s_subscribers[DATA_MANAGER_MAX_SUBSCRIBERS];
static void             *s_subscriber_user_data[DATA_MANAGER_MAX_SUBSCRIBERS];
static uint8_t           s_subscriber_count = 0;

static data_manager_stats_t s_stats;

/* ==================== Static helpers ==================== */

static data_quality_t determine_quality(const struct analysis_result *result)
{
    if (!result) return DATA_QUALITY_INVALID;

    if (result->samples_analyzed == 0 && result->temperature_valid) {
        return DATA_QUALITY_ENV_ONLY;
    }
    if (result->service_state == SENSOR_STATE_DEGRADED ||
        result->service_state == SENSOR_STATE_ERROR) {
        return DATA_QUALITY_DEGRADED;
    }
    return DATA_QUALITY_COMPLETE;
}

static void dispatch_callbacks(const system_data_packet_t *pkt)
{
    if (!pkt) return;

    /* Called OUTSIDE the mutex. Subscribers must not call back into
       data_manager functions that take the mutex (deadlock risk). */
    for (uint8_t i = 0; i < s_subscriber_count; i++) {
        if (s_subscribers[i]) {
            s_subscribers[i](pkt, s_subscriber_user_data[i]);
        }
    }
}

static void notify_consumers(void)
{
    uint8_t placeholder = 1;
    BaseType_t ret = xQueueSend(s_notify_q, &placeholder, 0);
    if (ret == pdTRUE) {
        s_stats.notify_sent++;
    } else {
        s_stats.notify_overflow++;
    }
}

/* ==================== Lifecycle API ==================== */

int data_manager_init(void)
{
    if (s_mutex) return APP_ERR_OK;  /* Already initialized */

    /* Create mutex */
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        LOG_ERROR(TAG, "Failed to create mutex");
        return APP_ERR_NO_MEM;
    }

    /* Create internal notification queue */
    s_notify_q = xQueueCreate(DATA_MANAGER_NOTIFYQ_DEPTH, sizeof(uint8_t));
    if (!s_notify_q) {
        LOG_ERROR(TAG, "Failed to create notification queue");
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return APP_ERR_NO_MEM;
    }

    /* Zero-initialize ring buffer */
    memset(s_ringbuf, 0, sizeof(s_ringbuf));
    memset(s_subscribers, 0, sizeof(s_subscribers));
    memset(&s_stats, 0, sizeof(s_stats));

    s_write_idx    = 0;
    s_sequence     = 0;
    s_stored_count = 0;
    s_subscriber_count = 0;

    LOG_INFO(TAG, "Data manager initialized (history=%u, notifyq=%u)",
             (unsigned)DATA_MANAGER_HISTORY, (unsigned)DATA_MANAGER_NOTIFYQ_DEPTH);
    return APP_ERR_OK;
}

int data_manager_deinit(void)
{
    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    if (s_notify_q) {
        vQueueDelete(s_notify_q);
        s_notify_q = NULL;
    }
    s_subscriber_count = 0;
    s_sequence = 0;
    s_stored_count = 0;
    memset(&s_stats, 0, sizeof(s_stats));
    return APP_ERR_OK;
}

/* ==================== Producer API ==================== */

int data_manager_push(const struct analysis_result *result)
{
    if (!s_mutex || !result) return APP_ERR_INVALID_PARAM;

    system_data_packet_t pkt;

    /* Build packet (before lock — minimize critical section) */
    pkt.arrival_timestamp_us = (uint64_t)esp_timer_get_time();
    pkt.quality = determine_quality(result);
    memcpy(&pkt.payload, result, sizeof(*result));

    /* Critical section: store in ring buffer */
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        s_stats.mutex_contention++;
        LOG_WARN(TAG, "Mutex contention on push (total=%lu)",
                 (unsigned long)s_stats.mutex_contention);
        return APP_ERR_BUSY;
    }

    pkt.sequence = s_sequence;
    memcpy(&s_ringbuf[s_write_idx], &pkt, sizeof(pkt));

    s_write_idx = (s_write_idx + 1) % DATA_MANAGER_HISTORY;
    s_sequence++;
    s_stored_count++;
    s_stats.total_pushes++;
    s_stats.total_packets = s_sequence;
    s_stats.last_push_timestamp_us = pkt.arrival_timestamp_us;

    if (s_stored_count > DATA_MANAGER_HISTORY) {
        s_stats.dropped_oldest = s_stored_count - DATA_MANAGER_HISTORY;
    }

    xSemaphoreGive(s_mutex);
    /* End critical section */

    /* Notify waiting consumers (outside lock) */
    notify_consumers();

    /* Dispatch subscriber callbacks (outside lock) */
    dispatch_callbacks(&pkt);

    return APP_ERR_OK;
}

/* ==================== Consumer API ==================== */

int data_manager_wait_for_new(uint32_t timeout_ms)
{
    if (!s_notify_q) return APP_ERR_GENERAL;

    uint8_t placeholder;
    TickType_t ticks = (timeout_ms == UINT32_MAX)
        ? portMAX_DELAY
        : pdMS_TO_TICKS(timeout_ms);

    BaseType_t ret = xQueueReceive(s_notify_q, &placeholder, ticks);
    if (ret == pdTRUE) {
        return APP_ERR_OK;
    }
    return APP_ERR_TIMEOUT;
}

int data_manager_get_latest(system_data_packet_t *out)
{
    if (!s_mutex || !out) return APP_ERR_INVALID_PARAM;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return APP_ERR_BUSY;
    }

    if (s_sequence == 0) {
        xSemaphoreGive(s_mutex);
        return APP_ERR_NO_DATA;
    }

    /* Latest is at (s_write_idx - 1) because s_write_idx points to next */
    uint32_t latest_idx = (s_write_idx == 0)
        ? DATA_MANAGER_HISTORY - 1
        : s_write_idx - 1;

    memcpy(out, &s_ringbuf[latest_idx], sizeof(*out));

    xSemaphoreGive(s_mutex);
    return APP_ERR_OK;
}

int data_manager_get_by_offset(uint32_t offset, system_data_packet_t *out)
{
    if (!s_mutex || !out) return APP_ERR_INVALID_PARAM;
    if (offset >= DATA_MANAGER_HISTORY) return APP_ERR_INVALID_PARAM;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return APP_ERR_BUSY;
    }

    uint32_t stored = (s_stored_count < DATA_MANAGER_HISTORY)
        ? s_stored_count
        : DATA_MANAGER_HISTORY;

    if (offset >= stored) {
        xSemaphoreGive(s_mutex);
        return APP_ERR_NO_DATA;
    }

    uint32_t idx = (s_write_idx + DATA_MANAGER_HISTORY - 1 - offset)
                   % DATA_MANAGER_HISTORY;

    memcpy(out, &s_ringbuf[idx], sizeof(*out));

    xSemaphoreGive(s_mutex);
    return APP_ERR_OK;
}

/* ==================== Subscription API ==================== */

int data_manager_subscribe(data_manager_cb_t cb, void *user_data)
{
    if (!cb) return APP_ERR_INVALID_PARAM;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return APP_ERR_BUSY;
    }

    if (s_subscriber_count >= DATA_MANAGER_MAX_SUBSCRIBERS) {
        xSemaphoreGive(s_mutex);
        LOG_WARN(TAG, "Subscriber slots full (max=%u)",
                 (unsigned)DATA_MANAGER_MAX_SUBSCRIBERS);
        return APP_ERR_NO_SPACE;
    }

    s_subscribers[s_subscriber_count] = cb;
    s_subscriber_user_data[s_subscriber_count] = user_data;
    s_subscriber_count++;
    s_stats.subscriber_count = s_subscriber_count;

    LOG_INFO(TAG, "Subscriber registered (total=%u)", (unsigned)s_subscriber_count);

    xSemaphoreGive(s_mutex);
    return APP_ERR_OK;
}

int data_manager_unsubscribe(data_manager_cb_t cb)
{
    if (!cb) return APP_ERR_INVALID_PARAM;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return APP_ERR_BUSY;
    }

    for (uint8_t i = 0; i < s_subscriber_count; i++) {
        if (s_subscribers[i] == cb) {
            /* Compact the array */
            uint8_t remaining = s_subscriber_count - i - 1;
            if (remaining > 0) {
                memmove(&s_subscribers[i], &s_subscribers[i + 1],
                        remaining * sizeof(data_manager_cb_t));
                memmove(&s_subscriber_user_data[i], &s_subscriber_user_data[i + 1],
                        remaining * sizeof(void *));
            }
            s_subscribers[s_subscriber_count - 1] = NULL;
            s_subscriber_user_data[s_subscriber_count - 1] = NULL;
            s_subscriber_count--;
            s_stats.subscriber_count = s_subscriber_count;
            xSemaphoreGive(s_mutex);
            LOG_INFO(TAG, "Subscriber unregistered (total=%u)",
                     (unsigned)s_subscriber_count);
            return APP_ERR_OK;
        }
    }

    xSemaphoreGive(s_mutex);
    return APP_ERR_GENERAL;  /* Not found */
}

/* ==================== Query API ==================== */

int data_manager_get_stats(data_manager_stats_t *stats)
{
    if (!stats) return APP_ERR_INVALID_PARAM;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return APP_ERR_BUSY;
    }

    memcpy(stats, &s_stats, sizeof(*stats));

    xSemaphoreGive(s_mutex);
    return APP_ERR_OK;
}

bool data_manager_has_data(void)
{
    if (!s_mutex) return false;

    bool has;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return false;
    }
    has = (s_sequence > 0);
    xSemaphoreGive(s_mutex);
    return has;
}

uint32_t data_manager_get_latest_sequence(void)
{
    /* Best-effort read without lock (uint32_t is atomic on ESP32) */
    return s_sequence;
}
