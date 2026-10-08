#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float scale;
    int32_t raw_average;
} shelf_calibration_result_t;

void shelf_service_init(void);
void shelf_service_start(void);

float shelf_service_get_primary_total(void);
/* Read-only: publish recent filtered measurements for every physical shelf. */
void shelf_service_send_all_snapshots(void);
void shelf_service_send_final_snapshots(void);
bool shelf_service_tare(uint8_t shelf_id);
bool shelf_service_calibrate(
    uint8_t shelf_id,
    float known_weight_g,
    shelf_calibration_result_t *result
);

/* Backward-compatible SHELF_1 APIs. */
void shelf_service_tare_primary(void);
void shelf_service_calibrate_primary(float known_weight_g);
void shelf_service_record_corner(uint8_t corner_number);
void shelf_service_report_corner_deviation(void);

#ifdef __cplusplus
}
#endif
