# 日志系统组件 (无 MQTT 循环依赖版本)
idf_component_register(
  SRCS "log_system.c"
  INCLUDE_DIRS "."
  REQUIRES common driver freertos mbedtls
  PRIV_REQUIRES unity
)

if(CONFIG_UNITY_ENABLE_BACKTRACE_ON_FAIL)
  target_sources(${COMPONENT_LIB} PRIVATE "log_system_test.c")
endif()
