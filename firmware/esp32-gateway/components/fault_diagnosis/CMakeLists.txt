# ISO 10816-3 rule-engine fallback for vibration fault classification
idf_component_register(
    SRCS "fault_diagnosis.c"
    INCLUDE_DIRS "."
    REQUIRES common log_system
)
