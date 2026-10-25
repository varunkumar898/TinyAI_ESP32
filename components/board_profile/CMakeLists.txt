idf_component_register(
    SRCS "board_profile.c"
    INCLUDE_DIRS "include"
    REQUIRES soc esp_system spi_flash
)
