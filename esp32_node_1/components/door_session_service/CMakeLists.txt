idf_component_register(
    SRCS "door_session_service.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES lock pi_uart
)
