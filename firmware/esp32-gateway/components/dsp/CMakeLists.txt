# 添加 DSP 组件
idf_component_register(
    SRCS "dsp.c"
    INCLUDE_DIRS "."
    REQUIRES driver common
)