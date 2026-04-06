 # 添加 ai_service 组件
 idf_component_register(
    SRCS "ai_service.c"
    INCLUDE_DIRS "."
    REQUIRES dsp common
 )