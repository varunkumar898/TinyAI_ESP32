idf_component_register(
    SRCS "config_store.c"
    INCLUDE_DIRS "include"
    REQUIRES pin_manager sensor_manager automation nvs_flash
)
