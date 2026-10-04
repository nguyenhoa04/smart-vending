#ifndef CALIBRATION_STORE_H
#define CALIBRATION_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * HX711 scale is stored as raw counts per gram.  The sign depends on the
 * load-cell wiring, therefore validation must use its absolute value.
 */
#define CALIBRATION_MIN_ABS_SCALE 0.5f

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t offset;
    float scale;
    bool offset_valid;
    bool scale_valid;
} calibration_store_data_t;

esp_err_t calibration_store_init(void);
esp_err_t calibration_store_load(
    uint8_t shelf_id,
    calibration_store_data_t *data
);
esp_err_t calibration_store_save(
    uint8_t shelf_id,
    const calibration_store_data_t *data
);
esp_err_t calibration_store_reset(uint8_t shelf_id);

#ifdef __cplusplus
}
#endif

#endif /* CALIBRATION_STORE_H */
