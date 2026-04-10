# time_sync 组件：企业级时间同步（SNTP网络同步/本地高精度计时器/自动周期同步）
#
# 依赖说明:
#   REQUIRES = 公共依赖 (头文件暴露给使用者)
#     - common         : 全局错误码、通用工具
#     - freertos       : FreeRTOS 任务/信号量/事件组
#     - esp_timer      : 高精度硬件定时器
#     - log_system     : 日志系统
#     - config_manager : 配置管理
#     - lwip           : LWIP 内置 SNTP 组件
#   PRIV_REQUIRES = 私有依赖（仅内部 .c / _test.c 实现）
#     - unity          : 单元测试框架 (仅用于 test 文件)

idf_component_register(
    SRCS "time_sync.c"
    INCLUDE_DIRS "."
    REQUIRES common freertos esp_timer log_system config_manager lwip
    PRIV_REQUIRES unity
)

target_sources(${COMPONENT_LIB} PRIVATE "time_sync_test.c")