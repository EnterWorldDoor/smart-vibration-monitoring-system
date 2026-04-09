idf_component_register(
    SRCS 
        esp32-gateway.c 
    INCLUDE_DIRS "."
                    REQUIRES adxl345 sensor_service time_sync log_system mqtt freertos esp_netif esp_event
                    PRIV_REQUIRES unity
                    )
