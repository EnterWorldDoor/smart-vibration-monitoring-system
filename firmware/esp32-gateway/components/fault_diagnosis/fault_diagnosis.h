/**
 * @file fault_diagnosis.h
 * @author EnterWorldDoor
 * @brief ISO 10816-3 rule-engine fallback for vibration fault classification
 *
 * Provides immediate fault diagnosis without ML model, used:
 *   - During ai_service cold start (first ~32s)
 *   - When primary model confidence < 0.85 (cascade fallback)
 *   - When TFLite model fails to load (degraded mode)
 *
 * ISO 10816-3 zones (Class II medium machines):
 *   Zone A (good):       < 1.4 mm/s
 *   Zone B (acceptable):  1.4 – 2.8 mm/s
 *   Zone C (alarm):       2.8 – 7.1 mm/s
 *   Zone D (danger):      > 7.1 mm/s
 */

#ifndef FAULT_DIAGNOSIS_H
#define FAULT_DIAGNOSIS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== Configuration ==================== */

#define FAULT_ISO_CLASS_II_ZONE_A    1.4f    /**< Zone A/B boundary (mm/s) */
#define FAULT_ISO_CLASS_II_ZONE_B    2.8f    /**< Zone B/C boundary (mm/s) */
#define FAULT_ISO_CLASS_II_ZONE_C    7.1f    /**< Zone C/D boundary (mm/s) */
#define FAULT_CREST_FACTOR_WARN      6.0f    /**< Crest factor warning threshold */
#define FAULT_KURTOSIS_WARN          4.0f    /**< Kurtosis warning threshold */
#define FAULT_TEMP_HIGH_WARN         85.0f   /**< Temperature warning (°C) */
#define FAULT_PEAK_FREQ_HIGH_WARN    500.0f  /**< High frequency warning (Hz) */

/* ==================== Type definitions ==================== */

typedef enum {
    FAULT_NORMAL = 0,
    FAULT_IMBALANCE = 1,
    FAULT_MISALIGNMENT = 2,
    FAULT_BEARING_FAULT = 3,
    FAULT_UNKNOWN = 4
} fault_type_t;

typedef struct {
    fault_type_t fault;
    float        confidence;
    char         fault_name[32];
    char         iso_zone;             /**< 'A', 'B', 'C', or 'D' */
    float        overall_rms_mm_s;
    uint8_t      triggered_rule_count;
} fault_diagnosis_t;

typedef struct {
    float zone_a_boundary;       /**< A/B threshold (mm/s) */
    float zone_b_boundary;       /**< B/C threshold (mm/s) */
    float zone_c_boundary;       /**< C/D threshold (mm/s) */
    float crest_factor_warn;
    float kurtosis_warn;
    float temp_high_warn;
    float peak_freq_high_warn;
} fault_diag_config_t;

/* ==================== API ==================== */

/**
 * @brief Initialize fault diagnosis rule engine.
 * @param config  NULL for ISO 10816 Class II defaults
 * @return APP_ERR_OK or error code
 */
int fault_diagnosis_init(const fault_diag_config_t *config);

/**
 * @brief Diagnose fault type from vibration statistics.
 *
 * @param overall_rms_mm_s  Overall RMS vibration in mm/s
 * @param peak_freq_hz      Dominant peak frequency in Hz
 * @param crest_factor      Crest factor (peak / rms)
 * @param kurtosis          Kurtosis of vibration distribution
 * @param temperature_c     Temperature in Celsius
 * @param out               Diagnosis result (must not be NULL)
 * @return APP_ERR_OK or error code
 */
int fault_diagnosis_diagnose(float overall_rms_mm_s,
                             float peak_freq_hz,
                             float crest_factor,
                             float kurtosis,
                             float temperature_c,
                             fault_diagnosis_t *out);

#ifdef __cplusplus
}
#endif

#endif /* FAULT_DIAGNOSIS_H */
