set(node_app_private_requires
    command_service
    door_session_service
    shelf_service
    pi_uart
)

idf_component_register(
    SRCS "node_app.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES ${node_app_private_requires}
)
