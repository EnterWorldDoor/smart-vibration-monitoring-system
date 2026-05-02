# 企业级传感器服务组件 (业务层核心)
idf_component_register(
    SRCS "sensor_service.c"
    INCLUDE_DIRS "."
    REQUIRES adxl345 dsp protocol temperature_compensation
            common freertos log_system esp_timer
)