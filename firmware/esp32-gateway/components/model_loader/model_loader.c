#include "model_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_spiffs.h"

#define TAG "model_loader"

#define MODELS_BASE_PATH "/models"
#define MODELS_PARTITION_LABEL "models"

bool g_spiffs_mounted = false;

esp_err_t model_loader_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = MODELS_BASE_PATH,
        .partition_label = MODELS_PARTITION_LABEL,
        .max_files = 5,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret == ESP_OK) {
        g_spiffs_mounted = true;

        size_t total = 0, used = 0;
        esp_spiffs_info(MODELS_PARTITION_LABEL, &total, &used);
        ESP_LOGI(TAG, "SPIFFS mounted: total=%u KB, used=%u KB", total / 1024, used / 1024);
    } else if (ret == ESP_FAIL) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS (partition 'models' not found?). "
                 "Check partitions.csv includes a 'models, data, spiffs' entry.");
    } else {
        ESP_LOGE(TAG, "SPIFFS mount error: 0x%x", ret);
    }

    return ret;
}

static void build_path(const char *filename, char *path, size_t path_size)
{
    snprintf(path, path_size, MODELS_BASE_PATH "/%s", filename);
}

bool model_loader_exists(const char *filename)
{
    if (!g_spiffs_mounted || filename == NULL) {
        return false;
    }

    char path[256];
    build_path(filename, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
        return true;
    }
    return false;
}

size_t model_loader_get_size(const char *filename)
{
    if (!g_spiffs_mounted || filename == NULL) {
        return 0;
    }

    char path[256];
    build_path(filename, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0;
    }

    fseek(f, 0, SEEK_END);
    size_t size = (size_t)ftell(f);
    fclose(f);
    return size;
}

esp_err_t model_loader_load(const char *filename, uint8_t **out_buf, size_t *out_len)
{
    if (!g_spiffs_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (filename == NULL || out_buf == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[256];
    build_path(filename, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "File not found: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    size_t file_size = (size_t)ftell(f);
    rewind(f);

    if (file_size == 0) {
        fclose(f);
        ESP_LOGE(TAG, "Empty file: %s", path);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buf = (uint8_t *)malloc(file_size);
    if (!buf) {
        fclose(f);
        ESP_LOGE(TAG, "malloc(%u) failed", (unsigned)file_size);
        return ESP_ERR_NO_MEM;
    }

    size_t read_bytes = fread(buf, 1, file_size, f);
    fclose(f);

    if (read_bytes != file_size) {
        free(buf);
        ESP_LOGE(TAG, "Short read: got %u, expected %u", (unsigned)read_bytes, (unsigned)file_size);
        return ESP_ERR_INVALID_SIZE;
    }

    *out_buf = buf;
    *out_len = file_size;

    ESP_LOGI(TAG, "Loaded %s (%u bytes)", filename, (unsigned)file_size);
    return ESP_OK;
}

void model_loader_free(uint8_t *buf)
{
    if (buf) {
        free(buf);
    }
}

esp_err_t model_loader_write(const char *filename, const uint8_t *data, size_t len)
{
    if (!g_spiffs_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (filename == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[256];
    build_path(filename, path, sizeof(path));

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open for write: %s", path);
        return ESP_FAIL;
    }

    size_t written = fwrite(data, 1, len, f);
    fclose(f);

    if (written != len) {
        ESP_LOGE(TAG, "Short write: %u/%u bytes", (unsigned)written, (unsigned)len);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Wrote %s (%u bytes)", filename, (unsigned)len);
    return ESP_OK;
}

esp_err_t model_loader_delete(const char *filename)
{
    if (!g_spiffs_mounted || filename == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[256];
    build_path(filename, path, sizeof(path));

    if (remove(path) != 0) {
        ESP_LOGE(TAG, "Delete failed: %s", path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Deleted %s", filename);
    return ESP_OK;
}
