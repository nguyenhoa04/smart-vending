idf_component_register(
    SRCS "lock.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_gpio
)