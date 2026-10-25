idf_component_register(
    SRCS "command_engine.c"
    INCLUDE_DIRS "include"
    REQUIRES pin_manager entity_extractor tinyml model_data
    PRIV_REQUIRES esp_timer
)
