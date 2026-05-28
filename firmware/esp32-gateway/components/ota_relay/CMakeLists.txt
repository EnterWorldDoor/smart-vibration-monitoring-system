idf_component_register(
    SRCS "ota_relay.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_client protocol global_error log_system mqtt_app config_manager
)
