# 添加 data_manager 组件
idf_component_register(
    SRCS "data_manager.c"
    INCLUDE_DIRS "."
    REQUIRES dsp sensor_service common ai_service fault_diagnosis time_sync freertos
)