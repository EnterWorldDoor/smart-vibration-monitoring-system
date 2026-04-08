 # 添加 ota_update 组件
 idf_component_register(
    SRCS "ota_update.c"
    INCLUDE_DIRS "."
    REQUIRES common 
 )