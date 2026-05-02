# DSP 组件 (基于 ESP-DSP 库或纯 C fallback 实现)
idf_component_register(
    SRCS "dsp.c"
    INCLUDE_DIRS "."
    REQUIRES driver common log_system esp_timer
)
