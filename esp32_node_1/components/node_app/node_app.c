#include "node_app.h"

#include "esp_log.h"

#include "command_service.h"
#include "door_session_service.h"
#include "pi_uart.h"
#include "shelf_service.h"

static const char *TAG = "MAIN_APP";

void node_app_start(void) {
    ESP_LOGI(TAG, "System is starting...");

    init_uart();
    shelf_service_init();
    door_session_service_init();

    shelf_service_start();
    door_session_service_start();
    command_service_start();
}
