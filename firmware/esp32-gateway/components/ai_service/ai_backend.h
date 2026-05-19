/**
 * @file ai_backend.h
 * @author EnterWorldDoor
 * @brief Pluggable inference backend abstraction
 *
 * Backends:
 *   - ai_backend_local  : TFLite Micro + cascade on ESP32 (implemented)
 *   - ai_backend_remote : Feature offload to Raspberry Pi via MQTT (stub)
 *
 * Usage:
 *   const ai_backend_t *backend = &ai_backend_local;
 *   backend->init();
 *   backend->infer(features, &result);
 */

#ifndef AI_BACKEND_H
#define AI_BACKEND_H

#include "ai_service.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** One-time initialization (arena alloc, interpreter setup) */
    int (*init)(void);

    /** Run inference on a 256×24 feature buffer, produce classification */
    int (*infer)(const float *features, ai_classification_t *out);

    /** Whether the backend is initialized and ready */
    bool (*is_ready)(void);

    /** Release backend resources */
    void (*deinit)(void);
} ai_backend_t;

extern const ai_backend_t ai_backend_local;
extern const ai_backend_t ai_backend_remote;

#ifdef __cplusplus
}
#endif

#endif /* AI_BACKEND_H */
