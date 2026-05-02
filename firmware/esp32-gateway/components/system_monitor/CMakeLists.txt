# system_monitor 组件：企业级系统监控（CPU/内存/任务/WDT/阈值告警/历史趋势）
idf_component_register(
    SRCS "system_monitor.c"
    INCLUDE_DIRS "."
    REQUIRES common log_system freertos
    PRIV_REQUIRES driver esp_timer esp_system unity
)

target_sources(${COMPONENT_LIB} PRIVATE "system_monitor_test.c")
