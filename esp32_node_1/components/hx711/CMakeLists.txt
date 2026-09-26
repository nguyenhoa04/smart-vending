idf_component_register(SRCS "hx711.c" 
                        INCLUDE_DIRS "include"
                        REQUIRES driver esp_timer esp_system esp_idf_lib_helpers)