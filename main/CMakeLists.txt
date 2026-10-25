idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES board_profile pin_manager sensor_manager automation
             command_engine serial_console config_store nvs_flash esp_timer
             esp_system freertos
)
