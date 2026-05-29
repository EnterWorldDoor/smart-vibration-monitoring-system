#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Mount the SPIFFS models partition.
 *
 * Must be called once before any other model_loader functions.
 * Mounts the "models" partition at /models path.
 *
 * @return ESP_OK on success.
 */
esp_err_t model_loader_init(void);

/**
 * @brief Check whether a model file exists on the models partition.
 */
bool model_loader_exists(const char *filename);

/**
 * @brief Get file size of a model file. Returns 0 if not found.
 */
size_t model_loader_get_size(const char *filename);

/**
 * @brief Load a model file from SPIFFS partition into a malloc'd buffer.
 *
 * @param filename  File name relative to /models (e.g. "esp32_classifier.tflite")
 * @param out_buf   Output: allocated buffer (caller must free with model_loader_free)
 * @param out_len   Output: buffer size in bytes
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if file doesn't exist.
 */
esp_err_t model_loader_load(const char *filename, uint8_t **out_buf, size_t *out_len);

/**
 * @brief Free a buffer allocated by model_loader_load.
 */
void model_loader_free(uint8_t *buf);

/**
 * @brief Write data to a file on the models partition. Overwrites if exists.
 *
 * @param filename  File name relative to /models
 * @param data      Data to write
 * @param len       Data length
 * @return ESP_OK on success.
 */
esp_err_t model_loader_write(const char *filename, const uint8_t *data, size_t len);

/**
 * @brief Delete a file from the models partition.
 */
esp_err_t model_loader_delete(const char *filename);

#ifdef __cplusplus
}
#endif
