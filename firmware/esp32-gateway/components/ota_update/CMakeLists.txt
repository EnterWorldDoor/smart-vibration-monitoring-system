# ota_update 组件：企业级 OTA 升级（HTTP下载/SHA256校验/版本检查/回滚/状态机）
#
# 依赖说明:
#   REQUIRES     = 公共依赖（头文件暴露给外部）
#     - common       : global_error.h
#     - log_system    : log_system/log_system.h
#     - config_manager: config_manager/config_manager.h (OTA配置参数)
#     - freertos     : FreeRTOS/task.h, semphr.h
#   PRIV_REQUIRES = 私有依赖（仅内部实现使用）
#     - esp_http_client : HTTP 客户端下载固件
#     - esp_system     : ESP 系统服务
#     - app_update     : OTA 操作接口 (esp_ota_ops.h)
#     - mbedtls        : SHA256 完整性校验
#     - unity          : 单元测试框架 (仅用于 test 文件)
#
idf_component_register(
    SRCS "ota_update.c"
    INCLUDE_DIRS "."
    REQUIRES common log_system config_manager freertos
    PRIV_REQUIRES esp_http_client esp_system app_update mbedtls unity
)

target_sources(${COMPONENT_LIB} PRIVATE "ota_update_test.c")