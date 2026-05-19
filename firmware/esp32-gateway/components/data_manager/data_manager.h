/**
 * @file data_manager.h
 * @author EnterWorldDoor
 * @brief 企业级数据管理器 — 系统数据总线, 融合原始数据、DSP、AI 结果、遥测
 *
 * Role:
 *   系统全局数据的单一可信源 (Single Source of Truth).
 *   sensor_service 推送完整分析结果, 所有下游消费者 (MQTT, monitor, OTA)
 *   通过统一接口获取数据包, 替代原始的 FreeRTOS Q2 队列.
 *
 * Architecture:
 *   ┌──────────────┐     push()     ┌──────────────┐  subscribe/wait  ┌──────────────┐
 *   │sensor_service│ ──────────────▶│ data_manager │─────────────────▶│ mqtt_app     │
 *   └──────────────┘                │              │                  │ monitor_task │
 *                                   │ ringbuf[8]  │                  │ future: OTA  │
 *                                   │ seq# counter│                  └──────────────┘
 *                                   │ mutex       │
 *                                   │ notifyq[4]  │
 *                                   └──────────────┘
 *
 * Key features:
 *   - 环形缓冲区历史 (N=8, ~16s @ 2s cadence)
 *   - 单调递增序列号 (gap detection)
 *   - 线程安全 (FreeRTOS mutex, copy-in/copy-out)
 *   - 订阅者回调 (最多 4 个)
 *   - 内部通知队列 (阻塞等待新数据, 替代 Q2)
 *   - 数据质量标记 (完整/降级/部分)
 */

#ifndef DATA_MANAGER_H
#define DATA_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "sensor_service.h"    /* struct analysis_result */

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== Configuration ==================== */

#define DATA_MANAGER_HISTORY       8      /**< Ring buffer depth (~16s history) */
#define DATA_MANAGER_MAX_SUBSCRIBERS 4    /**< Max callback subscribers */
#define DATA_MANAGER_NOTIFYQ_DEPTH 4      /**< Internal notification queue depth */

/* ==================== Type definitions ==================== */

typedef enum {
    DATA_QUALITY_COMPLETE  = 0,   /**< Full data: vibration + environment */
    DATA_QUALITY_DEGRADED  = 1,   /**< Degraded: no vibration, has environment */
    DATA_QUALITY_ENV_ONLY  = 2,   /**< Environment only, vibration invalid */
    DATA_QUALITY_INVALID   = 3    /**< Packet should not be consumed */
} data_quality_t;

/**
 * @brief 系统完整数据包 — extends analysis_result with metadata
 */
typedef struct {
    uint64_t arrival_timestamp_us;    /**< When data_manager received this (esp_timer) */
    uint32_t sequence;                /**< Monotonically increasing, gaps = lost data */
    data_quality_t quality;           /**< Data quality classification */
    struct analysis_result payload;   /**< The actual sensor + DSP + AI result */
} system_data_packet_t;

/**
 * @brief data_manager statistics
 */
typedef struct {
    uint32_t total_pushes;            /**< Total push() calls received */
    uint32_t total_packets;           /**< Total packets stored (sequence counter) */
    uint32_t dropped_oldest;          /**< Ring buffer overwrites */
    uint32_t subscriber_count;        /**< Active subscribers */
    uint32_t notify_sent;             /**< Notification signals sent */
    uint32_t notify_overflow;         /**< Notification queue full drops */
    uint32_t mutex_contention;        /**< Times mutex was held by someone else */
    uint64_t last_push_timestamp_us;  /**< Last push() timestamp */
} data_manager_stats_t;

/* ==================== Callback type ==================== */

/**
 * @brief Data packet subscriber callback.
 * @param pkt  Read-only pointer to the newly arrived packet.
 *             Valid only for the duration of this callback; copy if needed.
 * @param user_data  User context registered with subscribe().
 *
 * Called in the context of the producer (sensor_service task).
 * Do NOT block or take the data_manager mutex inside this callback.
 */
typedef void (*data_manager_cb_t)(const system_data_packet_t *pkt, void *user_data);

/* ==================== Lifecycle API ==================== */

/**
 * @brief Initialize data manager: ring buffer, mutex, notification queue.
 * @return APP_ERR_OK or error code
 */
int data_manager_init(void);

/**
 * @brief De-initialize and release all resources.
 * @return APP_ERR_OK or error code
 */
int data_manager_deinit(void);

/* ==================== Producer API ==================== */

/**
 * @brief Push a complete analysis result into the data bus.
 *
 * Called by sensor_service after DSP + AI processing.
 * Thread-safe, non-blocking (mutex held < 100µs for memcpy).
 *
 * Internally:
 *   1. Acquires mutex
 *   2. Wraps result into system_data_packet with sequence + quality + timestamp
 *   3. Stores in ring buffer
 *   4. Sends notification to waiting consumers
 *   5. Releases mutex
 *   6. Dispatches subscriber callbacks (outside mutex)
 *
 * @param result  Complete analysis result from sensor_service
 * @return APP_ERR_OK or APP_ERR_INVALID_PARAM
 */
int data_manager_push(const struct analysis_result *result);

/* ==================== Consumer API ==================== */

/**
 * @brief Block until new data is available or timeout expires.
 *
 * Used by MQTT upload task to wait efficiently instead of polling.
 * Each wake-up corresponds to one push() call.
 *
 * @param timeout_ms  Max wait time (ms), 0 = return immediately,
 *                    portMAX_DELAY = block indefinitely
 * @return APP_ERR_OK (data available), APP_ERR_TIMEOUT (no data within timeout)
 */
int data_manager_wait_for_new(uint32_t timeout_ms);

/**
 * @brief Get the most recent data packet (non-blocking).
 * @param out  Output packet (must not be NULL)
 * @return APP_ERR_OK or APP_ERR_NO_DATA (buffer empty)
 */
int data_manager_get_latest(system_data_packet_t *out);

/**
 * @brief Get a specific packet by ring buffer offset.
 * @param offset  0 = latest, 1 = previous, ..., DATA_MANAGER_HISTORY-1
 * @param out  Output packet
 * @return APP_ERR_OK or APP_ERR_INVALID_PARAM (offset out of range, or no data)
 */
int data_manager_get_by_offset(uint32_t offset, system_data_packet_t *out);

/* ==================== Subscription API ==================== */

/**
 * @brief Subscribe to new data packets via callback.
 * @param cb  Callback (called in producer context — do NOT block)
 * @param user_data  Opaque pointer passed to callback
 * @return APP_ERR_OK or APP_ERR_NO_SPACE (max subscribers reached)
 */
int data_manager_subscribe(data_manager_cb_t cb, void *user_data);

/**
 * @brief Unsubscribe a previously registered callback.
 */
int data_manager_unsubscribe(data_manager_cb_t cb);

/* ==================== Query API ==================== */

/**
 * @brief Get current statistics snapshot (non-blocking).
 */
int data_manager_get_stats(data_manager_stats_t *stats);

/**
 * @brief Check if the ring buffer has any data.
 */
bool data_manager_has_data(void);

/**
 * @brief Get the latest sequence number (for consumer gap detection).
 */
uint32_t data_manager_get_latest_sequence(void);

#ifdef __cplusplus
}
#endif

#endif /* DATA_MANAGER_H */
