/**
 * @file ai_service.h
 * @author EnterWorldDoor
 * @brief TinyML inference orchestrator — cascade architecture (primary CNN-LSTM + fallback ISO 10816)
 *
 * Architecture:
 *   sensor_service → ai_service_feed_frame(raw_samples) → feature buffer (256×24)
 *                  → ai_service_infer() → primary TFLite → confidence >= 0.85?
 *                                       → YES: classification output
 *                                       → NO:  fault_diagnosis fallback
 *
 * Cold start: fallback rule engine provides coverage during buffer warm-up.
 * Future: ai_backend_remote stub ready for Raspberry Pi offload.
 */

#ifndef AI_SERVICE_H
#define AI_SERVICE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== Configuration constants ==================== */

#define AI_WINDOW_SIZE          64       /**< Raw samples per feature window (power of 2) */
#define AI_WINDOW_STRIDE        32       /**< Sample stride between consecutive windows */
#define AI_FEATURE_WINDOWS      32       /**< Number of feature windows for model input (matches training TIME_STEPS) */
#define AI_NUM_FEATURES         24       /**< Feature vector dimension */
#define AI_CONFIDENCE_THRESHOLD 0.85f    /**< Cascade: primary → fallback threshold */
#define AI_INFERENCE_BUDGET_US  80000    /**< Max inference time budget (80ms) */

/* ==================== Type definitions ==================== */

typedef enum {
    AI_CLASS_NORMAL = 0,
    AI_CLASS_IMBALANCE = 1,
    AI_CLASS_MISALIGNMENT = 2,
    AI_CLASS_BEARING_FAULT = 3,
    AI_CLASS_UNCLASSIFIED = 255
} ai_class_id_t;

typedef struct {
    ai_class_id_t class_id;
    float         confidence;
    char          class_name[32];
    char          cascade_source[24];    /**< "primary_cnn", "fallback_rule", "fallback_coldstart" */
    uint32_t      inference_time_us;
} ai_classification_t;

typedef struct {
    uint32_t total_inferences;
    uint32_t primary_count;
    uint32_t fallback_count;
    uint32_t coldstart_count;
    uint32_t timeout_count;
    uint32_t error_count;
    float    avg_confidence;
    uint32_t avg_inference_time_us;
    uint64_t last_inference_timestamp_us;
} ai_health_t;

typedef struct {
    uint32_t window_size;
    uint32_t window_stride;
    uint32_t feature_windows;
    uint32_t num_features;
    float    confidence_threshold;
    uint32_t inference_budget_us;
} ai_config_t;

/* ==================== Lifecycle API ==================== */

/**
 * @brief Initialize AI service: TFLite interpreter, feature buffers, cascade.
 *        Falls back to fault_diagnosis-only on TFLite init failure.
 * @param config  NULL for defaults
 * @return APP_ERR_OK on success, negative error code on failure
 */
int ai_service_init(const ai_config_t *config);

/**
 * @brief De-initialize and release all resources.
 * @return APP_ERR_OK or error code
 */
int ai_service_deinit(void);

/* ==================== Data pipeline API ==================== */

/**
 * @brief Feed raw 3-axis samples into the feature extraction buffer.
 *        Called from sensor_service after each ADXL345 FIFO fetch.
 * @param x/y/z  Raw sample arrays (one float per sample, in g)
 * @param count  Number of samples per axis
 * @return APP_ERR_OK or APP_ERR_INVALID_PARAM
 */
int ai_service_feed_frame(const float *x, const float *y, const float *z, size_t count);

/**
 * @brief Set current temperature for feature extraction context.
 * @param temp_c  Temperature in Celsius (-50 to 150)
 */
void ai_service_set_temperature(float temp_c);

/**
 * @brief Run inference. During warm-up, uses fallback rule engine.
 *        Once feature buffer is full, runs full cascade (primary → fallback).
 * @param out  Classification result (always populated on success)
 * @return APP_ERR_OK or error code
 */
int ai_service_infer(ai_classification_t *out);

/**
 * @brief Check whether enough feature windows are buffered for primary inference.
 * @return true if 256 windows ready, false otherwise
 */
bool ai_service_is_ready(void);

/* ==================== Query API ==================== */

/**
 * @brief Get AI service health telemetry (read-only).
 * @return Pointer to health struct, never NULL after init
 */
const ai_health_t *ai_service_get_health(void);

/**
 * @brief Get number of feature windows currently buffered.
 * @return 0..256
 */
uint32_t ai_service_get_buffer_fill(void);

#ifdef __cplusplus
}
#endif

#endif /* AI_SERVICE_H */
