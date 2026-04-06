 # 添加 log_system 组件
 idf_component_register(
    SRCS "log_system.c"
    INCLUDE_DIRS "."
    REQUIRES common driver freertos 
 )   