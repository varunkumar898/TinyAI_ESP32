idf_component_register(
    SRCS "SensorManager.c"
    INCLUDE_DIRS "include"
    REQUIRES pin_manager
    PRIV_REQUIRES esp_adc esp_timer
)
