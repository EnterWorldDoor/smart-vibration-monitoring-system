# 添加 log_system 组件
idf_component_register(
  SRCS "log_system.c"
  INCLUDE_DIRS "."
  REQUIRES common driver freertos mqtt mbedtls
)

if(CONFIG_UNITY_ENABLE_BACKTRACE_ON_FAIL)
  target_sources(${COMPONENT_LIB} PRIVATE "log_system_test.c")
endif()
