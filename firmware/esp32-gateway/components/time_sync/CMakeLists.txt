idf_component_register(
    SRCS "time_sync.c"
    INCLUDE_DIRS "."
    REQUIRES common freertos esp_timer
)