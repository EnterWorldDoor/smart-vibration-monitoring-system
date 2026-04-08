 # 添加 fault_diagnosis 组件
 idf_component_register(
    SRCS "fault_diagnosis.c"
    INCLUDE_DIRS "."
    REQUIRES data_manager common
 )