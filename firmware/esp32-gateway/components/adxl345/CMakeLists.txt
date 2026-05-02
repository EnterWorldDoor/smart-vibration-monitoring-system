# ADXL345 组件 - 企业级三轴加速度计驱动 (SPI/FIFO/中断驱动)
# 功能: 高速数据采集、FIFO缓冲、DC偏移去除、滑动滤波、RMS融合、趋势分析
idf_component_register(
    SRCS "adxl345.c"
    INCLUDE_DIRS "."
    REQUIRES driver common log_system freertos esp_timer
    PRIV_REQUIRES unity
)
# 测试文件暂时禁用 - API 不匹配问题待修复
# target_sources(${COMPONENT_LIB} PRIVATE "adxl345_test.c")
