# 添加 sensor_service 组件
idf_component_register(
    SRCS "sensor_service.c"
    INCLUDE_DIRS "."
    REQUIRES adxl345 driver time_sync data_manager common freertos
)