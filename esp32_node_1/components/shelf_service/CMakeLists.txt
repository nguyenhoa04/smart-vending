idf_component_register(
    SRCS "shelf_service.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES hx711 pi_uart calibration_store door_session_service
)
