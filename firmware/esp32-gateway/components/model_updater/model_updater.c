#include "model_updater.h"
#include "model_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define TAG "model_updater"

#define NVS_NAMESPACE "models"
#define MAX_VERSION_LEN 32
#define MAX_HTTP_RX_BUF 4096
#define DOWNLOAD_TIMEOUT_MS 120000  /* 2 minutes for model download */

/* ------------------------------------------------------------------ */
/* Internal state                                                      */
/* ------------------------------------------------------------------ */

static char g_ota_server_url[128];
static char g_model_name[64];
static char g_current_version[MAX_VERSION_LEN];
static int g_check_interval_min = 60;
static model_updater_on_new_model_cb g_callback = NULL;
static TaskHandle_t g_task_handle = NULL;
static bool g_init_done = false;

/* ------------------------------------------------------------------ */
/* NVS helpers                                                         */
/* ------------------------------------------------------------------ */

static esp_err_t nvs_save_version(const char *model_name, const char *version)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    char key[64];
    snprintf(key, sizeof(key), "%s_ver", model_name);
    err = nvs_set_str(handle, key, version);
    if (err == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t model_updater_get_version(const char *model_name,
                                     char *version_out, size_t out_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    char key[64];
    snprintf(key, sizeof(key), "%s_ver", model_name);
    err = nvs_get_str(handle, key, version_out, &out_len);
    nvs_close(handle);
    return err;
}

/* ------------------------------------------------------------------ */
/* HTTP helpers                                                        */
/* ------------------------------------------------------------------ */

struct http_rx_buf {
    uint8_t *data;
    size_t   len;
    size_t   cap;
};

static esp_err_t http_rx_handler(esp_http_client_event_t *evt)
{
    struct http_rx_buf *rx = (struct http_rx_buf *)evt->user_data;
    switch (evt->event_type) {
    case HTTP_EVENT_ON_DATA: {
        size_t new_len = rx->len + evt->data_len;
        if (new_len > rx->cap) {
            size_t new_cap = rx->cap ? rx->cap * 2 : MAX_HTTP_RX_BUF;
            while (new_cap < new_len) new_cap *= 2;
            uint8_t *new_data = realloc(rx->data, new_cap);
            if (!new_data) {
                ESP_LOGE(TAG, "realloc(%u) failed", (unsigned)new_cap);
                return ESP_FAIL;
            }
            rx->data = new_data;
            rx->cap  = new_cap;
        }
        memcpy(rx->data + rx->len, evt->data, evt->data_len);
        rx->len = new_len;
        break;
    }
    default:
        break;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* JSON parsing                                                        */
/* ------------------------------------------------------------------ */

static bool parse_version_json(const char *json_data, int json_len,
                                const char *model_name,
                                char *version_out, size_t version_sz,
                                char *file_out, size_t file_sz,
                                char *sha256_out, size_t sha256_sz,
                                int *size_out)
{
    cJSON *root = cJSON_ParseWithLength(json_data, json_len);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed");
        return false;
    }

    cJSON *models = cJSON_GetObjectItem(root, "models");
    if (!models) {
        ESP_LOGW(TAG, "No 'models' section in version.json");
        cJSON_Delete(root);
        return false;
    }

    cJSON *model = cJSON_GetObjectItem(models, model_name);
    if (!model) {
        ESP_LOGW(TAG, "Model '%s' not found in version.json models section", model_name);
        cJSON_Delete(root);
        return false;
    }

    cJSON *ver = cJSON_GetObjectItem(model, "latest_version");
    cJSON *file = cJSON_GetObjectItem(model, "file");
    cJSON *sha  = cJSON_GetObjectItem(model, "sha256");
    cJSON *sz   = cJSON_GetObjectItem(model, "size");

    if (!cJSON_IsString(ver) || !cJSON_IsString(file)) {
        ESP_LOGE(TAG, "Invalid model entry: missing latest_version or file");
        cJSON_Delete(root);
        return false;
    }

    strncpy(version_out, ver->valuestring, version_sz - 1);
    strncpy(file_out, file->valuestring, file_sz - 1);

    if (sha && cJSON_IsString(sha)) {
        strncpy(sha256_out, sha->valuestring, sha256_sz - 1);
    }
    if (sz && cJSON_IsNumber(sz)) {
        *size_out = sz->valueint;
    }

    cJSON_Delete(root);
    return true;
}

/* ------------------------------------------------------------------ */
/* Download model file                                                 */
/* ------------------------------------------------------------------ */

static esp_err_t download_model(const char *file_name, uint8_t **out_data, size_t *out_len)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/firmware/esp32/%s",
             g_ota_server_url, file_name);

    ESP_LOGI(TAG, "Downloading: %s", url);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = DOWNLOAD_TIMEOUT_MS,
        .buffer_size = MAX_HTTP_RX_BUF,
        .event_handler = http_rx_handler,
    };

    struct http_rx_buf rx = { .data = NULL, .len = 0, .cap = 0 };
    cfg.user_data = &rx;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client init failed");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "HTTP download failed: err=%d, status=%d", err, status);
        free(rx.data);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Downloaded %u bytes", (unsigned)rx.len);
    *out_data = rx.data;
    *out_len  = rx.len;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* SHA-256 verification                                                */
/* ------------------------------------------------------------------ */

#include "mbedtls/sha256.h"

static bool verify_sha256(const uint8_t *data, size_t len,
                           const char *expected_hex)
{
    if (expected_hex[0] == '\0') {
        ESP_LOGW(TAG, "No SHA256 in version.json, skipping verification");
        return true;
    }

    uint8_t hash[32];
    mbedtls_sha256(data, len, hash, 0); /* 0 = SHA-256, not SHA-224 */

    char hash_hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(hash_hex + i * 2, 3, "%02x", hash[i]);
    }

    if (strcasecmp(hash_hex, expected_hex) != 0) {
        ESP_LOGE(TAG, "SHA256 mismatch! Expected: %s, Got: %s",
                 expected_hex, hash_hex);
        return false;
    }

    ESP_LOGI(TAG, "SHA256 verified");
    return true;
}

/* ------------------------------------------------------------------ */
/* Background task                                                     */
/* ------------------------------------------------------------------ */

static void model_updater_task(void *arg)
{
    uint32_t interval_ticks = g_check_interval_min * 60 * 1000 / portTICK_PERIOD_MS;

    while (1) {
        ESP_LOGI(TAG, "Checking for new model version...");

        /* Fetch version.json */
        char version_json_url[256];
        snprintf(version_json_url, sizeof(version_json_url),
                 "%s/firmware/version.json", g_ota_server_url);

        esp_http_client_config_t cfg = {
            .url = version_json_url,
            .timeout_ms = 15000,
            .buffer_size = MAX_HTTP_RX_BUF,
            .event_handler = http_rx_handler,
        };

        struct http_rx_buf rx = { .data = NULL, .len = 0, .cap = 0 };
        cfg.user_data = &rx;

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (client) {
            esp_err_t err = esp_http_client_perform(client);
            int status = esp_http_client_get_status_code(client);
            esp_http_client_cleanup(client);

            if (err == ESP_OK && status == 200 && rx.len > 0) {
                rx.data[rx.len] = '\0'; /* null-terminate for cJSON */

                char new_version[MAX_VERSION_LEN] = {0};
                char file_name[128] = {0};
                char sha256_hex[65] = {0};
                int file_size = 0;

                if (parse_version_json((const char *)rx.data, (int)rx.len,
                                       g_model_name,
                                       new_version, sizeof(new_version),
                                       file_name, sizeof(file_name),
                                       sha256_hex, sizeof(sha256_hex),
                                       &file_size)) {

                    if (g_current_version[0] != '\0' &&
                        strcmp(new_version, g_current_version) == 0) {
                        ESP_LOGI(TAG, "Already at latest version: %s", new_version);
                    } else {
                        ESP_LOGI(TAG, "New version available: %s (current: %s)",
                                 new_version, g_current_version[0] ? g_current_version : "none");

                        /* Download model */
                        uint8_t *model_data = NULL;
                        size_t model_len = 0;
                        if (download_model(file_name, &model_data, &model_len) == ESP_OK) {

                            if (verify_sha256(model_data, model_len, sha256_hex)) {

                                /* Write to SPIFFS */
                                esp_err_t write_err = model_loader_write(
                                    file_name, model_data, model_len);
                                if (write_err == ESP_OK) {
                                    /* Save version to NVS */
                                    nvs_save_version(g_model_name, new_version);
                                    strncpy(g_current_version, new_version, MAX_VERSION_LEN - 1);

                                    ESP_LOGI(TAG, "Model updated to %s", new_version);

                                    /* Notify callback */
                                    if (g_callback) {
                                        g_callback(g_model_name, new_version);
                                    }
                                }
                            }
                            free(model_data);
                        }
                    }
                }
            }
            free(rx.data);
        }

        /* Wait for next check interval */
        ulTaskNotifyTake(pdTRUE, interval_ticks);
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t model_updater_init(const char *ota_server_url,
                              const char *model_name,
                              const char *current_version,
                              int check_interval_minutes,
                              model_updater_on_new_model_cb callback)
{
    if (ota_server_url == NULL || model_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(g_ota_server_url, ota_server_url, sizeof(g_ota_server_url) - 1);
    strncpy(g_model_name, model_name, sizeof(g_model_name) - 1);

    if (current_version) {
        strncpy(g_current_version, current_version, MAX_VERSION_LEN - 1);
    }

    g_check_interval_min = check_interval_minutes > 0 ? check_interval_minutes : 60;
    g_callback = callback;

    /* Create background task */
    BaseType_t ret = xTaskCreate(
        model_updater_task,
        "model_updater",
        8192,   /* stack size: 8 KB */
        NULL,
        1,      /* priority: low */
        &g_task_handle
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create task");
        return ESP_FAIL;
    }

    g_init_done = true;
    ESP_LOGI(TAG, "Started: %s, check every %d min, current version: %s",
             model_name, check_interval_minutes,
             g_current_version[0] ? g_current_version : "none");

    return ESP_OK;
}

esp_err_t model_updater_check_now(void)
{
    if (!g_init_done || g_task_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xTaskNotifyGive(g_task_handle);
    return ESP_OK;
}
