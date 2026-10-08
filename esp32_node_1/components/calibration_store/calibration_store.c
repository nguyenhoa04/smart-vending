#include "calibration_store.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define CALIBRATION_MAGIC 0x43414C31UL
#define CALIBRATION_VERSION 1U
#define CALIBRATION_FLAG_OFFSET_VALID (1U << 0)
#define CALIBRATION_FLAG_SCALE_VALID  (1U << 1)

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    int32_t offset;
    float scale;
    uint8_t flags;
    uint8_t reserved[3];
} calibration_record_t;

static const char *TAG = "CAL_STORE";
static const char *NVS_NAMESPACE = "shelf_cal";
static bool store_ready;

static bool make_key(uint8_t shelf_id, char *key, size_t key_size) {
    if (shelf_id == 0 || key == NULL || key_size == 0) {
        return false;
    }

    int written = snprintf(key, key_size, "shelf_%u", shelf_id);
    return written > 0 && (size_t)written < key_size;
}

static bool record_is_valid(const calibration_record_t *record) {
    if (record == NULL ||
        record->magic != CALIBRATION_MAGIC ||
        record->version != CALIBRATION_VERSION ||
        record->size != (uint16_t)sizeof(*record)) {
        return false;
    }

    if ((record->flags & CALIBRATION_FLAG_SCALE_VALID) != 0U &&
        (!isfinite(record->scale) ||
         fabsf(record->scale) < CALIBRATION_MIN_ABS_SCALE)) {
        return false;
    }
    return true;
}

esp_err_t calibration_store_init(void) {
    if (store_ready) {
        return ESP_OK;
    }

    esp_err_t result = nvs_flash_init();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "NVS initialization failed: %s; preserving NVS, calibration requires maintenance",
                 esp_err_to_name(result));
        return result;
    }

    store_ready = true;
    ESP_LOGI(TAG, "Calibration NVS ready");
    return ESP_OK;
}

esp_err_t calibration_store_load(
    uint8_t shelf_id,
    calibration_store_data_t *data
) {
    if (!store_ready || data == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char key[16];
    if (!make_key(shelf_id, key, sizeof(key))) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (result != ESP_OK) {
        return result;
    }

    calibration_record_t record = {0};
    size_t record_size = sizeof(record);
    result = nvs_get_blob(handle, key, &record, &record_size);
    nvs_close(handle);

    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (result != ESP_OK) {
        return result;
    }
    if (record_size != sizeof(record) || !record_is_valid(&record)) {
        ESP_LOGE(TAG, "Invalid calibration record for SHELF_%u", shelf_id);
        return ESP_ERR_INVALID_VERSION;
    }

    data->offset = record.offset;
    data->scale = record.scale;
    data->offset_valid =
        (record.flags & CALIBRATION_FLAG_OFFSET_VALID) != 0U;
    data->scale_valid =
        (record.flags & CALIBRATION_FLAG_SCALE_VALID) != 0U;
    return ESP_OK;
}

esp_err_t calibration_store_save(
    uint8_t shelf_id,
    const calibration_store_data_t *data
) {
    if (!store_ready || data == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data->scale_valid &&
        (!isfinite(data->scale) ||
         fabsf(data->scale) < CALIBRATION_MIN_ABS_SCALE)) {
        return ESP_ERR_INVALID_ARG;
    }

    char key[16];
    if (!make_key(shelf_id, key, sizeof(key))) {
        return ESP_ERR_INVALID_ARG;
    }

    calibration_record_t record = {
        .magic = CALIBRATION_MAGIC,
        .version = CALIBRATION_VERSION,
        .size = (uint16_t)sizeof(calibration_record_t),
        .offset = data->offset,
        .scale = data->scale,
        .flags = 0,
        .reserved = {0},
    };
    if (data->offset_valid) {
        record.flags |= CALIBRATION_FLAG_OFFSET_VALID;
    }
    if (data->scale_valid) {
        record.flags |= CALIBRATION_FLAG_SCALE_VALID;
    }

    nvs_handle_t handle;
    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }

    result = nvs_set_blob(handle, key, &record, sizeof(record));
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);

    if (result == ESP_OK) {
        ESP_LOGI(TAG, "Saved calibration for SHELF_%u", shelf_id);
    } else {
        ESP_LOGE(
            TAG,
            "Could not save SHELF_%u calibration: %s",
            shelf_id,
            esp_err_to_name(result)
        );
    }
    return result;
}

esp_err_t calibration_store_reset(uint8_t shelf_id) {
    if (!store_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    char key[16];
    if (!make_key(shelf_id, key, sizeof(key))) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }

    result = nvs_erase_key(handle, key);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        result = ESP_OK;
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}
