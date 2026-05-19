/**
 * @file ai_backend_remote.c
 * @author EnterWorldDoor
 * @brief Remote inference backend stub — Raspberry Pi offload (future)
 *
 * Future workflow:
 *   ESP32 publishes features to MQTT topic → RPi receives → RPi runs inference
 *   → RPi publishes classification → ESP32 receives via MQTT subscription.
 *
 * This stub returns APP_ERR_NOT_SUPPORTED. Implement when RPi integration begins.
 */

#include "ai_backend.h"
#include "global_error.h"
#include "log_system.h"

static const char *TAG = "AI-REMOTE";

static int remote_init(void)
{
    LOG_INFO(TAG, "Remote backend (RPi offload) — not yet implemented");
    return APP_ERR_NOT_SUPPORTED;
}

static int remote_infer(const float *features, ai_classification_t *out)
{
    (void)features;
    (void)out;
    return APP_ERR_NOT_SUPPORTED;
}

static bool remote_is_ready(void)
{
    return false;
}

static void remote_deinit(void) {}

const ai_backend_t ai_backend_remote = {
    .init     = remote_init,
    .infer    = remote_infer,
    .is_ready = remote_is_ready,
    .deinit   = remote_deinit,
};
