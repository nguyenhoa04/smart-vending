#include "lock.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "hal/gpio_types.h"
#include "include/lock.h"
#include "soc/gpio_num.h"
#include <stdint.h>

#define RELAY_PIN  GPIO_NUM_4
static const char *TAG = "LOCK_TAG";


void lock_init(void){
    gpio_reset_pin(RELAY_PIN);
    gpio_set_direction(RELAY_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(RELAY_PIN, 0);
}

void lock_open(void){
    gpio_set_level(RELAY_PIN, 1);
    ESP_LOGI(TAG, "RELAY ON (UNLOCK)");
}

void lock_close(void){
    gpio_set_level(RELAY_PIN, 0);
    ESP_LOGI(TAG, "RELAY OFF (LOCK)");
}

void door_sensor_init(void){
    gpio_reset_pin(DOOR_SENSOR_PIN);
    gpio_set_direction(DOOR_SENSOR_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(DOOR_SENSOR_PIN, GPIO_PULLUP_ONLY);
}

bool is_door_closed(void){
    // Magnetic switch: close (0) when door closed (magnet near)
    // open (1) when door open (magnet away)
    return gpio_get_level(DOOR_SENSOR_PIN) == 0;
}