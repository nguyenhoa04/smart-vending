idf_component_register(
    SRCS "command_service.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES door_session_service shelf_service pi_uart
)
