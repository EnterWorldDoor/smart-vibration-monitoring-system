# TinyML inference orchestrator — cascade architecture
# esp-tflite-micro 位于 components/esp-tflite-micro (git clone)
idf_component_register(
    SRCS "ai_service.c"
         "ai_backend_local.cpp"
         "ai_backend_remote.c"
    INCLUDE_DIRS "."
    REQUIRES dsp common fault_diagnosis log_system freertos esp_timer esp-tflite-micro
)
