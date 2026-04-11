 # Protocol 组件 - 企业级 UART 通信协议 (STM32 ↔ ESP32)
# 功能: 帧解析/状态机/CRC16/ACK确认/心跳保活/序号机制/线程安全/统计监控
idf_component_register(
    SRCS "protocol.c"
    INCLUDE_DIRS "."
    REQUIRES common log_system driver freertos
    PRIV_REQUIRES unity
)

# 单元测试源文件
target_sources(${COMPONENT_LIB} PRIVATE "protocol_test.c")