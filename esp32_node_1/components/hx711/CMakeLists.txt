idf_component_register(
    SRCS "hx711.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_timer esp_system esp_idf_lib_helpers
)