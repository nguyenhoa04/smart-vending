#include "shelf_service.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "hx711.h"
#include "pi_uart.h"

#define SHELF_COUNT 2
#define MOVING_AVERAGE_WINDOW 20
#define TARE_SAMPLE_COUNT 20
#define HX711_READY_TIMEOUT_MS 200
#define CORNER_SAMPLE_COUNT 15
#define CORNER_COUNT 4

typedef struct {
    uint8_t id;
    hx711_config_t hx711;
    int32_t raw_value;
    float samples[MOVING_AVERAGE_WINDOW];
    uint8_t sample_count;
    uint8_t sample_index;
    float total_weight;
} shelf_runtime_t;

static const char *TAG = "MAIN_APP";

static shelf_runtime_t shelves[SHELF_COUNT] = {
    {
        .id = 1,
        .hx711 = {
            .id = 1,
            .dout_pin = GPIO_NUM_34,
            .sck_pin = GPIO_NUM_25,
            .offset = -186477,
            .scale = 32.0000f,
        },
    },
    {
        .id = 2,
        .hx711 = {
            .id = 2,
            .dout_pin = GPIO_NUM_35,
            .sck_pin = GPIO_NUM_26,
            .offset = 0,
            .scale = 1.0f,
        },
    },
};

static SemaphoreHandle_t shelf_mutex;
static float corner_readings_g[CORNER_COUNT] = {0};
static bool corner_has_reading[CORNER_COUNT] = {false, false, false, false};

static void sensor_task(void *context);
static shelf_runtime_t *find_active_shelf(uint8_t shelf_id);
static void reset_shelf_filter(shelf_runtime_t *shelf);
static bool read_selected_shelf_raw(shelf_runtime_t *shelf, int32_t *raw_value);
static bool tare_shelf(shelf_runtime_t *shelf);
static void update_shelf_weight_from_raw(shelf_runtime_t *shelf, int32_t raw_total);
static float update_shelf_moving_average(shelf_runtime_t *shelf, float sample_g);
static void send_shelf_uart_snapshot(const shelf_runtime_t *shelf);
static bool sample_shelf_average(
    shelf_runtime_t *shelf,
    uint8_t n_samples,
    float *out_grams
);
static bool calibrate_shelf_known_weight(
    shelf_runtime_t *shelf,
    float known_weight_g,
    shelf_calibration_result_t *result
);
static void record_corner_sample(uint8_t corner_index);

void shelf_service_init(void) {
    shelf_mutex = xSemaphoreCreateMutex();
    if (shelf_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create shelf mutex.");
    }

    for (uint8_t i = 0; i < SHELF_COUNT; ++i) {
        reset_shelf_filter(&shelves[i]);
        if (!hx711_init(&shelves[i].hx711)) {
            ESP_LOGE(
                TAG,
                "Failed to initialize SHELF_%u HX711 (SCK=%d, DOUT=%d)",
                shelves[i].id,
                shelves[i].hx711.sck_pin,
                shelves[i].hx711.dout_pin
            );
        }
    }

    ESP_LOGI(TAG, "Warming up %d independent HX711 converter(s) for 2 seconds...",
             SHELF_COUNT);
    vTaskDelay(pdMS_TO_TICKS(2000));

    for (uint8_t i = 0; i < 10; ++i) {
        for (uint8_t shelf_index = 0; shelf_index < SHELF_COUNT; ++shelf_index) {
            int32_t dummy = 0;
            if (!read_selected_shelf_raw(&shelves[shelf_index], &dummy)) {
                ESP_LOGW(
                    TAG,
                    "SHELF_%u timeout during startup discard %u",
                    shelves[shelf_index].id,
                    i + 1
                );
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    for (uint8_t i = 0; i < SHELF_COUNT; ++i) {
        ESP_LOGI(TAG, "Auto-taring shelf %u on startup...", shelves[i].id);
        (void)tare_shelf(&shelves[i]);
    }
}

void shelf_service_start(void) {
    xTaskCreate(sensor_task, "main_sensor_task", 4096, NULL, 5, NULL);
}

float shelf_service_get_primary_total(void) {
    return shelves[0].total_weight;
}

void shelf_service_tare_primary(void) {
    (void)shelf_service_tare(1);
}

void shelf_service_calibrate_primary(float known_weight_g) {
    (void)shelf_service_calibrate(1, known_weight_g, NULL);
}

bool shelf_service_tare(uint8_t shelf_id) {
    shelf_runtime_t *shelf = find_active_shelf(shelf_id);
    if (shelf == NULL) {
        return false;
    }

    bool success = false;
    if (shelf_mutex == NULL ||
        xSemaphoreTake(shelf_mutex, portMAX_DELAY) == pdTRUE) {
        success = tare_shelf(shelf);
        if (shelf_mutex != NULL) {
            xSemaphoreGive(shelf_mutex);
        }
    }
    return success;
}

bool shelf_service_calibrate(
    uint8_t shelf_id,
    float known_weight_g,
    shelf_calibration_result_t *result
) {
    shelf_runtime_t *shelf = find_active_shelf(shelf_id);
    if (shelf == NULL) {
        return false;
    }

    bool success = false;
    if (shelf_mutex == NULL ||
        xSemaphoreTake(shelf_mutex, portMAX_DELAY) == pdTRUE) {
        success = calibrate_shelf_known_weight(shelf, known_weight_g, result);
        if (shelf_mutex != NULL) {
            xSemaphoreGive(shelf_mutex);
        }
    }
    return success;
}

void shelf_service_record_corner(uint8_t corner_number) {
    if (corner_number == 0 || corner_number > CORNER_COUNT) {
        return;
    }

    if (shelf_mutex == NULL ||
        xSemaphoreTake(shelf_mutex, portMAX_DELAY) == pdTRUE) {
        record_corner_sample(corner_number - 1);
        if (shelf_mutex != NULL) {
            xSemaphoreGive(shelf_mutex);
        }
    }
}

void shelf_service_report_corner_deviation(void) {
    for (uint8_t i = 0; i < CORNER_COUNT; ++i) {
        if (!corner_has_reading[i]) {
            char payload[64];
            snprintf(
                payload,
                sizeof(payload),
                "CORNERCHECK: ERROR missing CORNER%d\n",
                i + 1
            );
            send_to_pi(payload);
            return;
        }
    }

    float sum = 0.0f;
    for (uint8_t i = 0; i < CORNER_COUNT; ++i) {
        sum += corner_readings_g[i];
    }
    float avg = sum / CORNER_COUNT;

    if (fabsf(avg) < 0.01f) {
        send_to_pi("CORNERCHECK: ERROR avg too small\n");
        return;
    }

    char payload[160];
    int offset = snprintf(
        payload,
        sizeof(payload),
        "CORNERCHECK: avg=%.1fg",
        avg
    );

    for (uint8_t i = 0; i < CORNER_COUNT && offset > 0 &&
                            (size_t)offset < sizeof(payload); ++i) {
        float dev_pct = (corner_readings_g[i] - avg) / avg * 100.0f;
        offset += snprintf(
            payload + offset,
            sizeof(payload) - (size_t)offset,
            " C%d=%.1f%%(%.1fg)",
            i + 1,
            dev_pct,
            corner_readings_g[i]
        );
        ESP_LOGI(
            TAG,
            "Corner %d: %.2f g | deviation %.2f%%",
            i + 1,
            corner_readings_g[i],
            dev_pct
        );
    }

    if (offset > 0 && (size_t)offset < sizeof(payload) - 1) {
        payload[offset++] = '\n';
        payload[offset] = '\0';
    } else {
        payload[sizeof(payload) - 2] = '\n';
        payload[sizeof(payload) - 1] = '\0';
    }
    send_to_pi(payload);
}

static shelf_runtime_t *find_active_shelf(uint8_t shelf_id) {
    if (shelf_id == 0 || shelf_id > SHELF_COUNT) {
        return NULL;
    }
    return &shelves[shelf_id - 1];
}

static bool read_selected_shelf_raw(shelf_runtime_t *shelf, int32_t *raw_value) {
    if (shelf == NULL || raw_value == NULL ||
        shelf->id == 0 || shelf->id > SHELF_COUNT) {
        return false;
    }
    return hx711_read_raw_timeout(&shelf->hx711, raw_value, HX711_READY_TIMEOUT_MS);
}

static void reset_shelf_filter(shelf_runtime_t *shelf) {
    shelf->raw_value = 0;
    shelf->sample_count = 0;
    shelf->sample_index = 0;
    shelf->total_weight = 0.0f;
    memset(shelf->samples, 0, sizeof(shelf->samples));
}

static bool tare_shelf(shelf_runtime_t *shelf) {
    if (shelf == NULL) {
        return false;
    }

    int64_t raw_sum = 0;
    uint8_t valid_samples = 0;

    for (uint8_t i = 0; i < TARE_SAMPLE_COUNT; ++i) {
        int32_t raw_value = 0;
        if (!read_selected_shelf_raw(shelf, &raw_value)) {
            ESP_LOGW(
                TAG,
                "SHELF_%d HX711 not ready during tare sample %d.",
                shelf->id,
                i + 1
            );
            continue;
        }

        raw_sum += raw_value;
        ++valid_samples;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (valid_samples == 0) {
        ESP_LOGW(TAG, "SHELF_%d tare skipped: no valid samples.", shelf->id);
        return false;
    }

    shelf->hx711.offset = (int32_t)(raw_sum / valid_samples);
    reset_shelf_filter(shelf);

    ESP_LOGI(
        TAG,
        "SHELF_%d tare complete | samples=%d | offset_total=%ld | scale_total=%.3f",
        shelf->id,
        valid_samples,
        (long)shelf->hx711.offset,
        shelf->hx711.scale
    );
    return true;
}

static void update_shelf_weight_from_raw(shelf_runtime_t *shelf, int32_t raw_total) {
    shelf->raw_value = raw_total;
    float calibrated_total = hx711_calibrate_raw(&shelf->hx711, raw_total);

    if (shelf->sample_count == MOVING_AVERAGE_WINDOW &&
        fabsf(calibrated_total - shelf->total_weight) > 10.0f) {
        for (uint8_t i = 0; i < MOVING_AVERAGE_WINDOW; ++i) {
            shelf->samples[i] = calibrated_total;
        }
    }

    float new_avg = update_shelf_moving_average(shelf, calibrated_total);
    if (new_avg >= -3.0f && new_avg <= 3.0f) {
        new_avg = 0.0f;
    }

    if (fabsf(new_avg - shelf->total_weight) > 5.0f ||
        shelf->sample_count < MOVING_AVERAGE_WINDOW) {
        shelf->total_weight = roundf(new_avg);
    }
}

static float update_shelf_moving_average(shelf_runtime_t *shelf, float sample_g) {
    shelf->samples[shelf->sample_index] = sample_g;
    shelf->sample_index = (shelf->sample_index + 1) % MOVING_AVERAGE_WINDOW;
    if (shelf->sample_count < MOVING_AVERAGE_WINDOW) {
        ++shelf->sample_count;
    }

    float sum = 0.0f;
    for (uint8_t i = 0; i < shelf->sample_count; ++i) {
        sum += shelf->samples[i];
    }

    float average = sum / (float)shelf->sample_count;
    if (average >= -1.0f && average <= 1.0f) {
        average = 0.0f;
    }
    return average;
}

static void send_shelf_uart_snapshot(const shelf_runtime_t *shelf) {
    char payload[64];
    snprintf(
        payload,
        sizeof(payload),
        "SHELF_%d: TOTAL=%.1f\n",
        shelf->id,
        shelf->total_weight
    );
    send_to_pi(payload);

    ESP_LOGI(
        TAG,
        "SHELF_%d -> TOTAL: %.1f | RAW: %ld | OFFSET: %ld",
        shelf->id,
        shelf->total_weight,
        (long)shelf->raw_value,
        (long)shelf->hx711.offset
    );
}

static bool sample_shelf_average(
    shelf_runtime_t *shelf,
    uint8_t n_samples,
    float *out_grams
) {
    int64_t raw_sum = 0;
    uint8_t valid_samples = 0;

    for (uint8_t i = 0; i < n_samples; ++i) {
        int32_t raw_value = 0;
        if (!read_selected_shelf_raw(shelf, &raw_value)) {
            continue;
        }
        raw_sum += raw_value;
        ++valid_samples;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (valid_samples == 0) {
        return false;
    }

    int32_t raw_avg = (int32_t)(raw_sum / valid_samples);
    *out_grams = hx711_calibrate_raw(&shelf->hx711, raw_avg);
    return true;
}

static bool calibrate_shelf_known_weight(
    shelf_runtime_t *shelf,
    float known_weight_g,
    shelf_calibration_result_t *result
) {
    if (shelf == NULL) {
        return false;
    }

    if (known_weight_g <= 0.0f) {
        ESP_LOGW(
            TAG,
            "SHELF_%d calibration has invalid known weight %.2f",
            shelf->id,
            known_weight_g
        );
        return false;
    }

    int64_t raw_sum = 0;
    uint8_t valid_samples = 0;
    for (uint8_t i = 0; i < TARE_SAMPLE_COUNT; ++i) {
        int32_t raw_value = 0;
        if (!read_selected_shelf_raw(shelf, &raw_value)) {
            continue;
        }
        raw_sum += raw_value;
        ++valid_samples;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (valid_samples == 0) {
        ESP_LOGW(
            TAG,
            "SHELF_%d calibration: no samples from HX711",
            shelf->id
        );
        return false;
    }

    int32_t raw_avg = (int32_t)(raw_sum / valid_samples);
    float new_scale =
        (float)(raw_avg - shelf->hx711.offset) / known_weight_g;

    if (fabsf(new_scale) < 0.0001f) {
        ESP_LOGW(
            TAG,
            "SHELF_%d calibration scale too small; check wiring and offset",
            shelf->id
        );
        return false;
    }

    shelf->hx711.scale = new_scale;
    reset_shelf_filter(shelf);

    if (result != NULL) {
        result->scale = new_scale;
        result->raw_average = raw_avg;
    }

    ESP_LOGI(
        TAG,
        "SHELF_%d calibrated | known=%.1fg | raw_avg=%ld | scale=%.4f",
        shelf->id,
        known_weight_g,
        (long)raw_avg,
        new_scale
    );
    return true;
}

static void record_corner_sample(uint8_t corner_index) {
    if (corner_index >= CORNER_COUNT) {
        return;
    }

    float grams = 0.0f;
    if (!sample_shelf_average(&shelves[0], CORNER_SAMPLE_COUNT, &grams)) {
        ESP_LOGW(TAG, "CORNER%d: no sample", corner_index + 1);
        char payload[48];
        snprintf(
            payload,
            sizeof(payload),
            "CORNER%d: ERROR no samples\n",
            corner_index + 1
        );
        send_to_pi(payload);
        return;
    }

    corner_readings_g[corner_index] = grams;
    corner_has_reading[corner_index] = true;

    char payload[48];
    snprintf(
        payload,
        sizeof(payload),
        "CORNER%d: %.1f g\n",
        corner_index + 1,
        grams
    );
    send_to_pi(payload);
    ESP_LOGI(TAG, "Corner %d reading = %.2f g", corner_index + 1, grams);
}

static void sensor_task(void *context) {
    (void)context;
    float last_printed[SHELF_COUNT] = {-9999.0f, -9999.0f};

    while (1) {
        if (shelf_mutex == NULL ||
            xSemaphoreTake(shelf_mutex, portMAX_DELAY) == pdTRUE) {
            for (uint8_t i = 0; i < SHELF_COUNT; ++i) {
                int32_t raw_value = 0;
                if (read_selected_shelf_raw(&shelves[i], &raw_value)) {
                    update_shelf_weight_from_raw(&shelves[i], raw_value);
                    if (fabsf(shelves[i].total_weight - last_printed[i]) > 0.1f) {
                        send_shelf_uart_snapshot(&shelves[i]);
                        last_printed[i] = shelves[i].total_weight;
                    }
                } else {
                    ESP_LOGW(
                        TAG,
                        "SHELF_%u HX711 timeout: check SCK GPIO%d and DOUT GPIO%d",
                        shelves[i].id,
                        shelves[i].hx711.sck_pin,
                        shelves[i].hx711.dout_pin
                    );
                }
            }

            if (shelf_mutex != NULL) {
                xSemaphoreGive(shelf_mutex);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
