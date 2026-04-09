# config_manager 组件：企业级配置管理器（NVS持久化 + Mutex线程安全 + CRC32校验）
#
# 依赖说明：
#   REQUIRES     = 公共依赖（头文件暴露给外部使用者）
#     - common       : global_error.h
#     - log_system    : log_system.h（被 config_manager.h 公开引用）
#     - freertos      : FreeRTOS/semphr.h（Mutex 线程安全，头文件可能被外部引用）
#   PRIV_REQUIRES = 私有依赖（仅内部 .c / _test.c 实现）
#     - nvs_flash     : nvs_flash.h / nvs.h（NVS 持久化，仅 .c 内部使用）
#     - unity         : unity.h（仅 test 文件使用）
#
idf_component_register(
    SRCS "config_manager.c"
    INCLUDE_DIRS "."
    REQUIRES common log_system freertos
    PRIV_REQUIRES nvs_flash unity
)

if(CONFIG_UNITY_ENABLE_BACKTRACE_ON_FAIL)
    target_sources(${COMPONENT_LIB} PRIVATE "config_manager_test.c")
endif()