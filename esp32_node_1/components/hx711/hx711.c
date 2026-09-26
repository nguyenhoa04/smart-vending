#include "hx711.h"

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define HX711_MAX_SHARED_CHANNELS 8
#define HX711_CLOCK_DELAY_US 5

static const char *TAG = "HX711";
static portMUX_TYPE s_hx711_spinlock = portMUX_INITIALIZER_UNLOCKED;

static bool hx711_pin_list_is_valid(
    gpio_num_t sck_pin,
    const gpio_num_t *dout_pins,
    size_t channel_count
) {
    if (!GPIO_IS_VALID_OUTPUT_GPIO(sck_pin) ||
        dout_pins == NULL ||
        channel_count == 0 ||
        channel_count > HX711_MAX_SHARED_CHANNELS) {
        return false;
    }

    for (size_t i = 0; i < channel_count; ++i) {
        if (!GPIO_IS_VALID_GPIO(dout_pins[i])) {
            return false;
        }
    }
    return true;
}

static bool hx711_all_channels_ready(
    const gpio_num_t *dout_pins,
    size_t channel_count
) {
    for (size_t i = 0; i < channel_count; ++i) {
        if (gpio_get_level(dout_pins[i]) != 0) {
            return false;
        }
    }
    return true;
}

bool hx711_init_shared(
    gpio_num_t sck_pin,
    const gpio_num_t *dout_pins,
    size_t channel_count
) {
    if (!hx711_pin_list_is_valid(sck_pin, dout_pins, channel_count)) {
        ESP_LOGE(TAG, "Invalid shared HX711 pin configuration");
        return false;
    }

    gpio_config_t sck_config = {
        .pin_bit_mask = 1ULL << sck_pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&sck_config) != ESP_OK) {
        ESP_LOGE(TAG, "Could not configure HX711 SCK GPIO%d", sck_pin);
        return false;
    }
    gpio_set_level(sck_pin, 0);

    uint64_t dout_mask = 0;
    for (size_t i = 0; i < channel_count; ++i) {
        dout_mask |= 1ULL << dout_pins[i];
    }

    gpio_config_t dout_config = {
        .pin_bit_mask = dout_mask,
        .mode = GPIO_MODE_INPUT,
        /* Input-only GPIOs on classic ESP32 have no internal pull resistors. */
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&dout_config) != ESP_OK) {
        ESP_LOGE(TAG, "Could not configure shared HX711 DOUT pins");
        return false;
    }

    ESP_LOGI(
        TAG,
        "HX711 interface ready: SCK=GPIO%d, channels=%u",
        sck_pin,
        (unsigned)channel_count
    );
    return true;
}

bool hx711_read_multi_raw_timeout(
    gpio_num_t sck_pin,
    const gpio_num_t *dout_pins,
    size_t channel_count,
    int32_t *raw_values,
    uint32_t timeout_ms
) {
    if (!hx711_pin_list_is_valid(sck_pin, dout_pins, channel_count) ||
        raw_values == NULL) {
        return false;
    }

    const int64_t deadline_us =
        esp_timer_get_time() + ((int64_t)timeout_ms * 1000LL);

    /* A shared SCK read may start only when every converter is ready. */
    while (!hx711_all_channels_ready(dout_pins, channel_count)) {
        if (esp_timer_get_time() >= deadline_us) {
            return false;
        }
        vTaskDelay(1);
    }

    uint32_t samples[HX711_MAX_SHARED_CHANNELS] = {0};

    /*
     * Keep the 24 data clocks and the gain-select clock contiguous. This also
     * prevents another task from stretching SCK HIGH long enough to put an
     * HX711 into power-down mode.
     */
    portENTER_CRITICAL(&s_hx711_spinlock);

    for (uint8_t bit = 0; bit < 24; ++bit) {
        gpio_set_level(sck_pin, 1);
        esp_rom_delay_us(HX711_CLOCK_DELAY_US);

        for (size_t channel = 0; channel < channel_count; ++channel) {
            samples[channel] =
                (samples[channel] << 1) |
                (uint32_t)gpio_get_level(dout_pins[channel]);
        }

        gpio_set_level(sck_pin, 0);
        esp_rom_delay_us(HX711_CLOCK_DELAY_US);
    }

    /* Pulse 25 selects channel A, gain 128 for the next conversion. */
    gpio_set_level(sck_pin, 1);
    esp_rom_delay_us(HX711_CLOCK_DELAY_US);
    gpio_set_level(sck_pin, 0);
    esp_rom_delay_us(HX711_CLOCK_DELAY_US);

    portEXIT_CRITICAL(&s_hx711_spinlock);

    for (size_t channel = 0; channel < channel_count; ++channel) {
        /* Sign-extend the HX711 signed 24-bit two's-complement result. */
        if ((samples[channel] & 0x00800000UL) != 0) {
            samples[channel] |= 0xFF000000UL;
        }
        raw_values[channel] = (int32_t)samples[channel];
    }

    return true;
}

bool hx711_init(hx711_config_t *config) {
    if (config == NULL) {
        return false;
    }
    return hx711_init_shared(config->sck_pin, &config->dout_pin, 1);
}

bool hx711_read_raw_timeout(
    const hx711_config_t *config,
    int32_t *raw_value,
    uint32_t timeout_ms
) {
    if (config == NULL) {
        return false;
    }
    return hx711_read_multi_raw_timeout(
        config->sck_pin,
        &config->dout_pin,
        1,
        raw_value,
        timeout_ms
    );
}

float hx711_calibrate_raw(const hx711_config_t *config, int32_t raw_value) {
    if (config == NULL || config->scale == 0.0f) {
        return 0.0f;
    }
    return ((float)raw_value - (float)config->offset) / config->scale;
}
