#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t id;
    gpio_num_t dout_pin;
    gpio_num_t sck_pin;
    int32_t offset;
    float scale;
} hx711_config_t;

/* API for one HX711 with a private SCK line. */
bool hx711_init(hx711_config_t *config);
bool hx711_read_raw_timeout(
    const hx711_config_t *config,
    int32_t *raw_value,
    uint32_t timeout_ms
);

/*
 * Shared-clock API used by the SmartVending RevF PCB.
 *
 * One SCK drives every HX711. Each conversion is shifted out at the same
 * instant, so all DOUT pins must be sampled during every SCK pulse.
 */
bool hx711_init_shared(
    gpio_num_t sck_pin,
    const gpio_num_t *dout_pins,
    size_t channel_count
);

bool hx711_read_multi_raw_timeout(
    gpio_num_t sck_pin,
    const gpio_num_t *dout_pins,
    size_t channel_count,
    int32_t *raw_values,
    uint32_t timeout_ms
);

float hx711_calibrate_raw(const hx711_config_t *config, int32_t raw_value);

#ifdef __cplusplus
}
#endif
