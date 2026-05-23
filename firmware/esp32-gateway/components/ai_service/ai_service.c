/**
 * @file ai_service.c
 * @author EnterWorldDoor
 * @brief TinyML inference orchestrator implementation
 *
 * Data flow:
 *   ai_service_feed_frame(x,y,z,count) → raw buffer → feature extraction (DSP)
 *   → feature buffer[256×24] → ai_service_infer() → cascade (primary/fallback)
 *
 * Cold start: fault_diagnosis fallback during feature buffer warm-up (~32s).
 */

#include "ai_service.h"
#include "ai_backend.h"
#include "dsp.h"
#include "global_error.h"
#include "fault_diagnosis.h"
#include "log_system.h"
#include "esp_timer.h"
#include <string.h>
#include <math.h>

static const char *TAG = "AI";

/* ==================== Internal state (static allocation) ==================== */

#define AI_RAW_BUFFER_SIZE      1024    /**< Must be >= max batch size from sensor_service */

/* Raw sample linear buffer */
static float s_raw_x[AI_RAW_BUFFER_SIZE];
static float s_raw_y[AI_RAW_BUFFER_SIZE];
static float s_raw_z[AI_RAW_BUFFER_SIZE];
static size_t s_raw_count = 0;

/* DSP work buffers (dsp_fft_compute_3axis modifies input in place) */
static float s_dsp_wx[AI_WINDOW_SIZE];
static float s_dsp_wy[AI_WINDOW_SIZE];
static float s_dsp_wz[AI_WINDOW_SIZE];

/* Feature buffer — circular, AI_FEATURE_WINDOWS × AI_NUM_FEATURES */
static float s_feature_buf[AI_FEATURE_WINDOWS][AI_NUM_FEATURES];
static uint32_t s_feat_write_idx = 0;   /* next write position */
static uint32_t s_feat_count = 0;       /* 0..AI_FEATURE_WINDOWS */

/* Flattened input for TFLite inference (pre-allocated, avoids stack overflow) */
static float s_inference_input[AI_FEATURE_WINDOWS * AI_NUM_FEATURES];

/* Cached vibration stats for cold start fallback (updated after each feature extraction) */
static float s_cached_overall_rms  = 0.0f;
static float s_cached_peak_freq    = 0.0f;
static float s_cached_crest_factor = 0.0f;
static float s_cached_kurtosis     = 0.0f;
static float s_current_temp_c      = 25.0f;

/* Backend, health and state */
static const ai_backend_t *s_backend = NULL;
static ai_health_t s_health;
static ai_config_t s_config;
static bool s_initialized = false;
static bool s_backend_ready = false;

/* ==================== Static helpers ==================== */

static void reset_health(void)
{
    memset(&s_health, 0, sizeof(s_health));
}

/**
 * @brief Extract 24 features from dsp_3axis_result and push to feature buffer.
 *        Also updates cached vibration stats for cold start diagnostics.
 *
 * Feature layout (must match training pipeline):
 *   [0]  rms_x           [8]  crest_factor_x    [16] band_energy_x[7]
 *   [1]  rms_y           [9]  band_energy_x[0]  [17] peak_freq_y
 *   [2]  rms_z           [10] band_energy_x[1]  [18] peak_amp_y
 *   [3]  overall_rms     [11] band_energy_x[2]  [19] crest_factor_y
 *   [4]  peak_freq_x     [12] band_energy_x[3]  [20] peak_freq_z
 *   [5]  peak_amp_x      [13] band_energy_x[4]  [21] peak_amp_z
 *   [6]  skewness_x      [14] band_energy_x[5]  [22] crest_factor_z
 *   [7]  kurtosis_x      [15] band_energy_x[6]  [23] temperature_c
 */
static void push_feature_vector(const struct dsp_3axis_result *dsp)
{
    float *slot = s_feature_buf[s_feat_write_idx];

    slot[0]  = dsp->x_rms.value;
    slot[1]  = dsp->y_rms.value;
    slot[2]  = dsp->z_rms.value;
    slot[3]  = dsp->vector_rms.value;
    slot[4]  = dsp->x_fft.peak_freq;
    slot[5]  = dsp->x_fft.peak_amp;
    slot[6]  = dsp->x_rms.skewness;
    slot[7]  = dsp->x_rms.kurtosis;
    slot[8]  = dsp->x_rms.crest_factor;
    for (int b = 0; b < 8; b++) slot[9 + b]  = dsp->x_fft.band_energy[b];
    slot[17] = dsp->y_fft.peak_freq;
    slot[18] = dsp->y_fft.peak_amp;
    slot[19] = dsp->y_rms.crest_factor;
    slot[20] = dsp->z_fft.peak_freq;
    slot[21] = dsp->z_fft.peak_amp;
    slot[22] = dsp->z_rms.crest_factor;
    slot[23] = s_current_temp_c;

    /* Update cached stats for cold start fallback */
    s_cached_overall_rms  = dsp->vector_rms.value;
    s_cached_peak_freq    = fmaxf(fmaxf(dsp->x_fft.peak_freq, dsp->y_fft.peak_freq),
                                  dsp->z_fft.peak_freq);
    s_cached_crest_factor = fmaxf(fmaxf(dsp->x_rms.crest_factor, dsp->y_rms.crest_factor),
                                  dsp->z_rms.crest_factor);
    s_cached_kurtosis     = fmaxf(fmaxf(dsp->x_rms.kurtosis, dsp->y_rms.kurtosis),
                                  dsp->z_rms.kurtosis);

    /* Advance circular buffer */
    s_feat_write_idx = (s_feat_write_idx + 1) % AI_FEATURE_WINDOWS;
    if (s_feat_count < AI_FEATURE_WINDOWS) {
        s_feat_count++;
    }
}

/**
 * @brief Extract features from accumulated raw samples.
 *        Processes as many windows as available given the stride.
 */
static void extract_features_from_raw(void)
{
    while (s_raw_count >= s_config.window_size) {
        /* Copy raw samples to DSP work buffers */
        for (size_t i = 0; i < s_config.window_size; i++) {
            s_dsp_wx[i] = s_raw_x[i];
            s_dsp_wy[i] = s_raw_y[i];
            s_dsp_wz[i] = s_raw_z[i];
        }

        struct dsp_3axis_result dsp_result;
        int ret = dsp_fft_compute_3axis(s_dsp_wx, s_dsp_wy, s_dsp_wz,
                                         (uint16_t)s_config.window_size,
                                         400.0f, DSP_WINDOW_HANN, &dsp_result);
        if (ret != APP_ERR_OK) {
            LOG_ERROR(TAG, "Feature extraction DSP failed: %d", ret);
            s_health.error_count++;
            return;
        }

        push_feature_vector(&dsp_result);

        /* Slide raw buffer forward by stride */
        size_t remaining = s_raw_count - s_config.window_stride;
        if (remaining > 0) {
            memmove(s_raw_x, s_raw_x + s_config.window_stride,
                    remaining * sizeof(float));
            memmove(s_raw_y, s_raw_y + s_config.window_stride,
                    remaining * sizeof(float));
            memmove(s_raw_z, s_raw_z + s_config.window_stride,
                    remaining * sizeof(float));
        }
        s_raw_count = remaining;
    }
}

/**
 * @brief Flatten circular feature buffer into contiguous input for TFLite.
 *        Result placed in s_inference_input (statically allocated).
 */
static void flatten_features(void)
{
    /* chronological order: oldest first */
    uint32_t oldest_idx = (s_feat_count == AI_FEATURE_WINDOWS)
        ? s_feat_write_idx   /* wrap point = oldest when full */
        : 0;

    for (uint32_t i = 0; i < s_feat_count; i++) {
        uint32_t src = (oldest_idx + i) % AI_FEATURE_WINDOWS;
        memcpy(&s_inference_input[i * AI_NUM_FEATURES],
               s_feature_buf[src],
               AI_NUM_FEATURES * sizeof(float));
    }
}

/**
 * @brief Run fallback fault diagnosis and map to ai_classification_t.
 */
static void run_fallback(ai_classification_t *out, const char *source)
{
    fault_diagnosis_t diag;
    int ret = fault_diagnosis_diagnose(
        s_cached_overall_rms, s_cached_peak_freq,
        s_cached_crest_factor, s_cached_kurtosis,
        s_current_temp_c, &diag);

    if (ret == APP_ERR_OK) {
        out->class_id  = (ai_class_id_t)diag.fault;
        out->confidence = diag.confidence;
        strncpy(out->class_name, diag.fault_name, sizeof(out->class_name) - 1);
    } else {
        out->class_id  = AI_CLASS_UNCLASSIFIED;
        out->confidence = 0.0f;
        strncpy(out->class_name, "unclassified", sizeof(out->class_name) - 1);
    }

    strncpy(out->cascade_source, source, sizeof(out->cascade_source) - 1);
    out->inference_time_us = 0;

    /* Distinguish coldstart from cascade fallback by source string */
    if (source[6] == 'c') {   /* "fallback_coldstart" */
        s_health.coldstart_count++;
    } else {
        s_health.fallback_count++;
    }
    s_health.total_inferences++;
    s_health.last_inference_timestamp_us = esp_timer_get_time();
}

/* ==================== Lifecycle API ==================== */

int ai_service_init(const ai_config_t *config)
{
    if (s_initialized) return APP_ERR_OK;

    /* Apply config */
    if (config) {
        memcpy(&s_config, config, sizeof(s_config));
    } else {
        s_config = (ai_config_t) {
            .window_size          = AI_WINDOW_SIZE,
            .window_stride        = AI_WINDOW_STRIDE,
            .feature_windows      = AI_FEATURE_WINDOWS,
            .num_features         = AI_NUM_FEATURES,
            .confidence_threshold = AI_CONFIDENCE_THRESHOLD,
            .inference_budget_us  = AI_INFERENCE_BUDGET_US,
        };
    }

    /* Zero-initialize all static buffers */
    memset(s_raw_x, 0, sizeof(s_raw_x));
    memset(s_raw_y, 0, sizeof(s_raw_y));
    memset(s_raw_z, 0, sizeof(s_raw_z));
    memset(s_feature_buf, 0, sizeof(s_feature_buf));
    memset(s_inference_input, 0, sizeof(s_inference_input));

    /* Init fault diagnosis fallback */
    fault_diagnosis_init(NULL);

    /* Try primary backend */
    s_backend = &ai_backend_local;
    if (s_backend->init() == APP_ERR_OK && s_backend->is_ready()) {
        s_backend_ready = true;
        LOG_INFO(TAG, "Primary backend (TFLite) initialized");
    } else {
        s_backend_ready = false;
        LOG_WARN(TAG, "Primary backend unavailable — running fallback-only");
    }

    reset_health();
    s_raw_count = 0;
    s_feat_write_idx = 0;
    s_feat_count = 0;
    s_initialized = true;

    LOG_INFO(TAG, "AI service initialized (window=%u, stride=%u, feat_windows=%u)",
             (unsigned)s_config.window_size, (unsigned)s_config.window_stride,
             (unsigned)s_config.feature_windows);
    return APP_ERR_OK;
}

int ai_service_deinit(void)
{
    if (s_backend && s_backend->deinit) {
        s_backend->deinit();
    }
    s_backend = NULL;
    s_initialized = false;
    s_backend_ready = false;
    s_raw_count = 0;
    s_feat_write_idx = 0;
    s_feat_count = 0;
    return APP_ERR_OK;
}

/* ==================== Data pipeline API ==================== */

int ai_service_feed_frame(const float *x, const float *y, const float *z,
                          size_t count)
{
    if (!s_initialized || !x || !y || !z || count == 0) {
        return APP_ERR_INVALID_PARAM;
    }

    /* If buffer would overflow, drop oldest data */
    if (s_raw_count + count > AI_RAW_BUFFER_SIZE) {
        size_t overflow = (s_raw_count + count) - AI_RAW_BUFFER_SIZE;
        if (overflow < s_raw_count) {
            memmove(s_raw_x, s_raw_x + overflow,
                    (s_raw_count - overflow) * sizeof(float));
            memmove(s_raw_y, s_raw_y + overflow,
                    (s_raw_count - overflow) * sizeof(float));
            memmove(s_raw_z, s_raw_z + overflow,
                    (s_raw_count - overflow) * sizeof(float));
            s_raw_count -= overflow;
        } else {
            s_raw_count = 0;
        }
    }

    /* Append new samples */
    memcpy(s_raw_x + s_raw_count, x, count * sizeof(float));
    memcpy(s_raw_y + s_raw_count, y, count * sizeof(float));
    memcpy(s_raw_z + s_raw_count, z, count * sizeof(float));
    s_raw_count += count;

    /* Extract features from accumulated raw data */
    extract_features_from_raw();

    return APP_ERR_OK;
}

void ai_service_set_temperature(float temp_c)
{
    if (!isnan(temp_c) && temp_c > -50.0f && temp_c < 150.0f) {
        s_current_temp_c = temp_c;
    }
}

int ai_service_infer(ai_classification_t *out)
{
    if (!s_initialized || !out) return APP_ERR_INVALID_PARAM;
    memset(out, 0, sizeof(*out));

    /* Cold start: feature buffer not yet full → fallback */
    if (s_feat_count < s_config.feature_windows) {
        run_fallback(out, "fallback_coldstart");
        return APP_ERR_OK;
    }

    /* Primary inference through backend */
    if (s_backend_ready && s_backend && s_backend->infer) {
        flatten_features();

        int64_t start_us = esp_timer_get_time();
        int ret = s_backend->infer(s_inference_input, out);
        int64_t elapsed_us = esp_timer_get_time() - start_us;

        out->inference_time_us = (uint32_t)elapsed_us;

        /* Update health */
        s_health.total_inferences++;
        s_health.last_inference_timestamp_us = esp_timer_get_time();
        s_health.avg_confidence = (s_health.avg_confidence *
            (float)(s_health.total_inferences - 1) + out->confidence)
            / (float)s_health.total_inferences;
        s_health.avg_inference_time_us = (s_health.avg_inference_time_us *
            (uint32_t)(s_health.total_inferences - 1) + out->inference_time_us)
            / (uint32_t)s_health.total_inferences;

        if (ret != APP_ERR_OK) {
            LOG_WARN(TAG, "Primary inference failed (err=%d), falling back", ret);
            s_health.error_count++;
            run_fallback(out, "fallback_rule");
            return APP_ERR_OK;
        }

        /* Check timeout */
        if (elapsed_us > (int64_t)s_config.inference_budget_us) {
            LOG_WARN(TAG, "Inference timeout: %lld us (budget %u us)",
                     (long long)elapsed_us, (unsigned)s_config.inference_budget_us);
            s_health.timeout_count++;
        }

        /* Cascade: low confidence → fallback */
        if (out->confidence < s_config.confidence_threshold) {
            LOG_DEBUG(TAG, "Low confidence %.3f < %.3f, cascading to fallback",
                      (double)out->confidence, (double)s_config.confidence_threshold);
            s_health.fallback_count++;
            run_fallback(out, "fallback_rule");
        } else {
            strncpy(out->cascade_source, "primary_cnn", sizeof(out->cascade_source) - 1);
            s_health.primary_count++;
        }

        return APP_ERR_OK;
    }

    /* Backend not available → fallback */
    run_fallback(out, "fallback_rule");
    return APP_ERR_OK;
}

/* ==================== Query API ==================== */

bool ai_service_is_ready(void)
{
    return s_initialized && s_feat_count >= s_config.feature_windows;
}

const ai_health_t *ai_service_get_health(void)
{
    return &s_health;
}

uint32_t ai_service_get_buffer_fill(void)
{
    return s_feat_count;
}

int ai_service_get_latest_features(float *features_out)
{
    if (!features_out || s_feat_count == 0)
        return -1;

    uint32_t latest_idx = (s_feat_write_idx + AI_FEATURE_WINDOWS - 1) % AI_FEATURE_WINDOWS;
    memcpy(features_out, s_feature_buf[latest_idx], AI_NUM_FEATURES * sizeof(float));
    return 0;
}
