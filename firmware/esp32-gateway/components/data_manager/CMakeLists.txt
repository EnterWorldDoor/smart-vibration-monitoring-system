# 企业级数据管理器 — 系统数据总线, 融合原始数据、DSP、AI 结果、遥测
idf_component_register(
    SRCS "data_manager.c"
    INCLUDE_DIRS "."
    REQUIRES sensor_service common log_system freertos esp_timer
)
