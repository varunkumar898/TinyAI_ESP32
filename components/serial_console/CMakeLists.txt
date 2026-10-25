idf_component_register(
    SRCS "SerialConsole.c"
    INCLUDE_DIRS "include"
    REQUIRES pin_manager command_engine sensor_manager automation config_store board_profile
    PRIV_REQUIRES esp_timer esp_driver_uart esp_system
)
