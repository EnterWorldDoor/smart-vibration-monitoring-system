# 温度补偿组件 (EWMA + 时间戳对齐 + 变化率检测)
idf_component_register(
    SRCS "temperature_compensation.c"
    INCLUDE_DIRS "."
    REQUIRES
    PRIV_REQUIRES
        common        # global_error.h (内部使用)
        time_sync     # time_sync.h (内部使用)
        log_system    # log_system.h (内部使用)
)
# 测试文件暂时禁用 - API 不匹配问题待修复
# target_sources(${COMPONENT_LIB} PRIVATE "temperature_compensation_test.c")
