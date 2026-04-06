 # 添加 config_manager 组件
 idf_component_register(
    SRCS "config_manager.c"
    INCLUDE_DIRS "."
    REQUIRES common 
 )