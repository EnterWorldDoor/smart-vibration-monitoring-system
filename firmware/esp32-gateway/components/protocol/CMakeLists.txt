 # 添加 protocol 组件 - 用于和stm32进行通信的(基于 uart协议)
 idf_component_register(
    SRCS "protocol.c"
    INCLUDE_DIRS "."
    REQUIRES common driver freertos
 )