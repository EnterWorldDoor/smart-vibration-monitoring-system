idf_component_register(
    SRCS "model_updater.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_http_client cJSON model_loader log
)
