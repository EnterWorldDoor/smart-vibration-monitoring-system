#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Callback invoked when a new model version is downloaded and validated. */
typedef void (*model_updater_on_new_model_cb)(const char *model_name,
                                               const char *version);

/**
 * @brief Initialize the model updater background task.
 *
 * @param ota_server_url          Base URL of OTA server (e.g. "http://192.168.1.1:8090")
 * @param model_name              Model to watch (e.g. "esp32_classifier")
 * @param current_version         Currently loaded version (from NVS). Can be "" if unknown.
 * @param check_interval_minutes  How often to poll version.json
 * @param callback                Called when a new model is downloaded and SHA256-verified.
 *                                Runs in the model_updater task context.
 * @return ESP_OK on success.
 */
esp_err_t model_updater_init(const char *ota_server_url,
                              const char *model_name,
                              const char *current_version,
                              int check_interval_minutes,
                              model_updater_on_new_model_cb callback);

/**
 * @brief Trigger an immediate version check (bypasses the timer).
 */
esp_err_t model_updater_check_now(void);

/**
 * @brief Get the currently active model version from NVS.
 *
 * @param model_name  Model name
 * @param version_out Buffer for version string (at least 32 bytes)
 * @param out_len     Size of version_out buffer
 * @return ESP_OK if version is stored, ESP_ERR_NOT_FOUND otherwise.
 */
esp_err_t model_updater_get_version(const char *model_name,
                                     char *version_out, size_t out_len);

#ifdef __cplusplus
}
#endif
