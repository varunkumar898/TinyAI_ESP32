idf_component_register(
    SRCS "Automation.c" "button_fsm.c"
    INCLUDE_DIRS "include"
    REQUIRES pin_manager sensor_manager
)
