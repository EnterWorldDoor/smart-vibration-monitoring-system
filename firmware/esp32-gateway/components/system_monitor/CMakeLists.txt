 # 添加 system_monitor 组件
 idf_component_register(
    SRCS "system_monitor.c"
    INCLUDE_DIRS "."
    REQUIRES common freertos 
 )