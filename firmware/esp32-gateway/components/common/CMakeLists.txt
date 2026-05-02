 # Common 组件 - 通用工具库 (ringbuf 环形缓冲区等)
# 功能: 企业级环形缓冲区、线程安全、统计监控
idf_component_register(
   SRCS "ringbuf.c"
   INCLUDE_DIRS "."
   REQUIRES freertos
   PRIV_REQUIRES unity
)

# 单元测试源文件
target_sources(${COMPONENT_LIB} PRIVATE "ringbuf_test.c")