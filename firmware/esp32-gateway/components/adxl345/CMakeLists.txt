# 添加 ADXL345 组件
idf_component_register(
    SRCS "adxl345.c"
    INCLUDE_DIRS "."
    REQUIRES driver common log_system
)