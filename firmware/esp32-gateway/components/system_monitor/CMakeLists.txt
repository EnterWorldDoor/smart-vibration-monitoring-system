# system_monitor 组件：企业级系统监控（CPU/内存/任务/WDT/阈值告警/历史趋势）
#
# 依赖说明:
#   REQUIRES     = 公共依赖（头文件暴露给外部）
#     - common       : global_error.h
#     - log_system    : log_system/log_system.h
#     - freertos     : FreeRTOS/semphr.h, task.h
#   PRIV_REQUIRES = 私有依赖（仅内部实现使用）
#     - driver       : esp_timer.h (系统运行时间)
#     - esp_task_wdt : 看门狗喂狗接口
#     - unity        : 单元测试框架 (仅用于 test 文件)
#
idf_component_register(
    SRCS "system_monitor.c"
    INCLUDE_DIRS "."
    REQUIRES common log_system freertos
    PRIV_REQUIRES driver esp_system unity
)

target_sources(${COMPONENT_LIB} PRIVATE "system_monitor_test.c")