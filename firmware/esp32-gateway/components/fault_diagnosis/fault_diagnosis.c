/**
 * @file fault_diagnosis.c
 * @author EnterWorldDoor
 * @brief ISO 10816-3 rule-engine implementation
 *
 * Dependency: common (global_error.h)
 */

#include "fault_diagnosis.h"
#include "global_error.h"
#include "log_system.h"
#include <string.h>
#include <math.h>

static const char *TAG = "FAULT";
static fault_diag_config_t s_config;
static bool s_initialized = false;

/* ==================== Lifecycle ==================== */

int fault_diagnosis_init(const fault_diag_config_t *config)
{
    if (config) {
        memcpy(&s_config, config, sizeof(s_config));
    } else {
        s_config = (fault_diag_config_t) {
            .zone_a_boundary     = FAULT_ISO_CLASS_II_ZONE_A,
            .zone_b_boundary     = FAULT_ISO_CLASS_II_ZONE_B,
            .zone_c_boundary     = FAULT_ISO_CLASS_II_ZONE_C,
            .crest_factor_warn   = FAULT_CREST_FACTOR_WARN,
            .kurtosis_warn       = FAULT_KURTOSIS_WARN,
            .temp_high_warn      = FAULT_TEMP_HIGH_WARN,
            .peak_freq_high_warn = FAULT_PEAK_FREQ_HIGH_WARN,
        };
    }
    s_initialized = true;
    LOG_INFO(TAG, "Initialized (ISO Class II: A=%.1f B=%.1f C=%.1f mm/s)",
             (double)s_config.zone_a_boundary,
             (double)s_config.zone_b_boundary,
             (double)s_config.zone_c_boundary);
    return APP_ERR_OK;
}

/* ==================== Zone classification ==================== */

static char classify_iso_zone(float rms_mm_s)
{
    if (rms_mm_s >= s_config.zone_c_boundary) return 'D';
    if (rms_mm_s >= s_config.zone_b_boundary) return 'C';
    if (rms_mm_s >= s_config.zone_a_boundary) return 'B';
    return 'A';
}

/* ==================== Fault type inference ==================== */

static fault_type_t infer_fault_type(float overall_rms_mm_s,
                                     float peak_freq_hz,
                                     float crest_factor,
                                     float kurtosis,
                                     float temperature_c,
                                     char iso_zone,
                                     uint8_t *triggered_count)
{
    uint8_t n_triggers = 0;
    fault_type_t primary_fault = FAULT_NORMAL;

    /* Zone-based baseline */
    if (iso_zone == 'D') {
        primary_fault = FAULT_BEARING_FAULT;
        n_triggers++;
    } else if (iso_zone == 'C') {
        primary_fault = FAULT_MISALIGNMENT;
        n_triggers++;
    } else if (iso_zone == 'B') {
        primary_fault = FAULT_IMBALANCE;
        n_triggers++;
    }

    /* High frequency → bearing fault indication */
    if (peak_freq_hz > s_config.peak_freq_high_warn) {
        LOG_WARN(TAG, "High frequency detected: %.1f Hz", (double)peak_freq_hz);
        primary_fault = FAULT_BEARING_FAULT;
        n_triggers++;
    }

    /* High crest factor → shock / imbalance */
    if (crest_factor > s_config.crest_factor_warn) {
        LOG_WARN(TAG, "High crest factor: %.2f", (double)crest_factor);
        if (primary_fault == FAULT_NORMAL || primary_fault == FAULT_IMBALANCE) {
            primary_fault = FAULT_IMBALANCE;
        }
        n_triggers++;
    }

    /* High kurtosis → heavy-tailed distribution */
    if (kurtosis > s_config.kurtosis_warn) {
        n_triggers++;
    }

    /* High temperature */
    if (temperature_c > s_config.temp_high_warn) {
        LOG_WARN(TAG, "High temperature: %.1f °C", (double)temperature_c);
        n_triggers++;
    }

    *triggered_count = n_triggers;
    return primary_fault;
}

/* ==================== Confidence calculation ==================== */

static float calculate_confidence(fault_type_t fault, char iso_zone, uint8_t n_triggers)
{
    if (fault == FAULT_NORMAL && iso_zone == 'A') {
        return 0.95f;
    }

    float base = 0.5f;
    float zone_factor = 0.0f;
    switch (iso_zone) {
        case 'D': zone_factor = 0.35f; break;
        case 'C': zone_factor = 0.25f; break;
        case 'B': zone_factor = 0.15f; break;
        default:  zone_factor = 0.05f; break;
    }

    float trigger_factor = fminf(0.2f, n_triggers * 0.05f);

    float conf = base + zone_factor + trigger_factor;
    if (conf > 1.0f) conf = 1.0f;
    if (conf < 0.3f) conf = 0.3f;
    return conf;
}

/* ==================== Public API ==================== */

int fault_diagnosis_diagnose(float overall_rms_mm_s,
                             float peak_freq_hz,
                             float crest_factor,
                             float kurtosis,
                             float temperature_c,
                             fault_diagnosis_t *out)
{
    if (!s_initialized) return APP_ERR_GENERAL;
    if (!out) return APP_ERR_INVALID_PARAM;
    if (isnan(overall_rms_mm_s) || isnan(peak_freq_hz)) return APP_ERR_INVALID_PARAM;

    memset(out, 0, sizeof(*out));

    char iso_zone = classify_iso_zone(overall_rms_mm_s);
    uint8_t n_triggers = 0;
    fault_type_t fault = infer_fault_type(overall_rms_mm_s, peak_freq_hz,
                                          crest_factor, kurtosis, temperature_c,
                                          iso_zone, &n_triggers);

    static const char *fault_names[] = {
        "normal", "imbalance", "misalignment", "bearing_fault", "unknown"
    };

    out->fault             = fault;
    out->confidence         = calculate_confidence(fault, iso_zone, n_triggers);
    out->iso_zone           = iso_zone;
    out->overall_rms_mm_s   = overall_rms_mm_s;
    out->triggered_rule_count = n_triggers;
    strncpy(out->fault_name,
            fault_names[fault <= FAULT_UNKNOWN ? fault : FAULT_UNKNOWN],
            sizeof(out->fault_name) - 1);

    return APP_ERR_OK;
}
