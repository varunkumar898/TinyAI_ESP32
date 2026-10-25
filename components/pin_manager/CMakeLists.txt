idf_component_register(
    SRCS "PinManager.c"
    INCLUDE_DIRS "include"
    REQUIRES baremetal_gpio board_profile
    PRIV_REQUIRES esp_timer
)
