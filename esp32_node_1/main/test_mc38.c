#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "TEST_MC38";

// Thay đổi chân GPIO này nếu bạn cắm dây MC-38 vào chân khác
#define DOOR_SENSOR_PIN GPIO_NUM_5

void app_main(void)
{
    ESP_LOGI(TAG, "Khởi tạo chân cảm biến MC-38...");

    // Cấu hình chân GPIO làm input và kéo lên (Pull-up)
    gpio_reset_pin(DOOR_SENSOR_PIN);
    gpio_set_direction(DOOR_SENSOR_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(DOOR_SENSOR_PIN, GPIO_PULLUP_ONLY);

    ESP_LOGI(TAG, "Bắt đầu đọc trạng thái cảm biến (chu kỳ 500ms):");

    int last_state = -1;

    while (1) {
        // Đọc mức logic của chân GPIO
        // Nếu dùng Pull-up và nối 1 đầu MC-38 vào GND:
        // - Khi 2 mảnh gần nhau (cửa đóng) -> Công tắc đóng -> Nối GND -> Mức LOW (0)
        // - Khi 2 mảnh xa nhau (cửa mở)   -> Công tắc mở   -> Trở kéo lên -> Mức HIGH (1)
        int current_state = gpio_get_level(DOOR_SENSOR_PIN);

        // Chỉ in ra log khi có sự thay đổi trạng thái
        if (current_state != last_state) {
            if (current_state == 0) {
                ESP_LOGI(TAG, "Trạng thái: ĐÓNG CỬA (Nam châm LẠI GẦN) - Logic 0");
            } else {
                ESP_LOGI(TAG, "Trạng thái: MỞ CỬA (Nam châm RA XA) - Logic 1");
            }
            last_state = current_state;
        }

        // Đợi 100ms trước khi đọc lại
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
