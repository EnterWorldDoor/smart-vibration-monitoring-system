# ota_update 组件：企业级 OTA 升级（HTTP下载/SHA256校验/版本检查/回滚/状态机）
idf_component_register(
    SRCS "ota_update.c"
    INCLUDE_DIRS "."
    REQUIRES common log_system config_manager freertos
    PRIV_REQUIRES esp_http_client esp_timer esp_system app_update mbedtls unity
)

target_sources(${COMPONENT_LIB} PRIVATE "ota_update_test.c")
