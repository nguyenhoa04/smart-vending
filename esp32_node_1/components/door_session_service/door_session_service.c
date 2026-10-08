#include "door_session_service.h"

#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lock.h"
#include "pi_uart.h"

#define DOOR_POLL_INTERVAL_MS 20
#define DOOR_DEBOUNCE_SAMPLES 3
#define UNLOCK_TIMEOUT_MS 5000
#define DOOR_SESSION_TASK_STACK_SIZE 3072
#define DOOR_SESSION_TASK_PRIORITY 5

static const char *TAG = "DOOR_SESSION";

static SemaphoreHandle_t state_mutex;
static door_session_state_t session_state = DOOR_SESSION_IDLE;
static TickType_t unlock_started_at;
static bool lock_unlocked;
static bool stable_door_closed;
static bool candidate_door_closed;
static uint8_t candidate_sample_count;
static bool close_boundary_valid;
static TickType_t close_boundary_at;

static void door_session_task(void *context);
static bool update_debounced_door_state(bool raw_door_closed);
static void process_door_state(void);
static void close_lock_if_needed(void);

void door_session_service_init(void) {
    state_mutex = xSemaphoreCreateMutex();
    if (state_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create state mutex");
    }

    lock_init();
    door_sensor_init();

    stable_door_closed = is_door_closed();
    candidate_door_closed = stable_door_closed;
    candidate_sample_count = 0;
    lock_unlocked = false;
    session_state = DOOR_SESSION_IDLE;
    close_boundary_valid = false;

    ESP_LOGI(
        TAG,
        "MC-38 ready | GPIO=%d | raw=%d | door=%s | lock=locked | session=idle",
        DOOR_SENSOR_PIN,
        stable_door_closed ? 0 : 1,
        stable_door_closed ? "closed" : "open"
    );
}

void door_session_service_start(void) {
    xTaskCreate(
        door_session_task,
        "DOOR_SESSION",
        DOOR_SESSION_TASK_STACK_SIZE,
        NULL,
        DOOR_SESSION_TASK_PRIORITY,
        NULL
    );
}

void door_session_service_request_unlock(void) {
    if (state_mutex == NULL ||
        xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE) {
        lock_open();
        lock_unlocked = true;
        unlock_started_at = xTaskGetTickCount();

        if (session_state != DOOR_SESSION_ACTIVE) {
            session_state = DOOR_SESSION_WAITING_FOR_OPEN;
        }

        send_to_pi("LOCK: unlocked\n");

        if (state_mutex != NULL) {
            xSemaphoreGive(state_mutex);
        }
    }
}

void door_session_service_get_status(door_session_status_t *status) {
    if (status == NULL) {
        return;
    }

    if (state_mutex == NULL ||
        xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE) {
        status->door_closed = stable_door_closed;
        status->lock_unlocked = lock_unlocked;
        status->session_state = session_state;

        if (state_mutex != NULL) {
            xSemaphoreGive(state_mutex);
        }
    }
}

const char *door_session_service_state_name(door_session_state_t state) {
    switch (state) {
        case DOOR_SESSION_WAITING_FOR_OPEN:
            return "waiting_for_open";
        case DOOR_SESSION_ACTIVE:
            return "active";
        case DOOR_SESSION_IDLE:
        default:
            return "idle";
    }
}

bool door_session_service_get_close_boundary(uint32_t *boundary_tick) {
    if (boundary_tick == NULL || state_mutex == NULL ||
        xSemaphoreTake(state_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    bool valid = close_boundary_valid && stable_door_closed && !lock_unlocked;
    if (valid) *boundary_tick = close_boundary_at;
    xSemaphoreGive(state_mutex);
    return valid;
}

static bool update_debounced_door_state(bool raw_door_closed) {
    if (raw_door_closed != candidate_door_closed) {
        candidate_door_closed = raw_door_closed;
        candidate_sample_count = 1;
        return false;
    }

    if (candidate_sample_count < DOOR_DEBOUNCE_SAMPLES) {
        ++candidate_sample_count;
    }

    if (candidate_sample_count < DOOR_DEBOUNCE_SAMPLES ||
        stable_door_closed == candidate_door_closed) {
        return false;
    }

    stable_door_closed = candidate_door_closed;
    return true;
}

static void close_lock_if_needed(void) {
    if (!lock_unlocked) {
        return;
    }

    lock_close();
    lock_unlocked = false;
    send_to_pi("LOCK: locked\n");
}

static void process_door_state(void) {
    bool door_changed = update_debounced_door_state(is_door_closed());
    if (door_changed) {
        close_boundary_valid = stable_door_closed && session_state == DOOR_SESSION_ACTIVE;
        if (close_boundary_valid) close_boundary_at = xTaskGetTickCount();
        ESP_LOGI(
            TAG,
            "MC-38 changed | GPIO=%d | raw=%d | door=%s",
            DOOR_SENSOR_PIN,
            stable_door_closed ? 0 : 1,
            stable_door_closed ? "closed" : "open"
        );
        send_to_pi(stable_door_closed ? "DOOR: closed\n" : "DOOR: opened\n");
    }

    TickType_t unlock_elapsed = xTaskGetTickCount() - unlock_started_at;
    bool unlock_timed_out =
        lock_unlocked && unlock_elapsed >= pdMS_TO_TICKS(UNLOCK_TIMEOUT_MS);

    switch (session_state) {
        case DOOR_SESSION_WAITING_FOR_OPEN:
            if (!stable_door_closed) {
                session_state = DOOR_SESSION_ACTIVE;
                send_to_pi("SESSION: started\n");
                ESP_LOGI(TAG, "Purchase session started");
            }

            if (unlock_timed_out) {
                close_lock_if_needed();
                if (session_state == DOOR_SESSION_WAITING_FOR_OPEN) {
                    session_state = DOOR_SESSION_IDLE;
                    ESP_LOGI(TAG, "Unlock timed out before door opened");
                }
            }
            break;

        case DOOR_SESSION_ACTIVE:
            if (unlock_timed_out) {
                close_lock_if_needed();
            }

            if (stable_door_closed) {
                close_lock_if_needed();
                session_state = DOOR_SESSION_IDLE;
                send_to_pi("SESSION: ended\n");
                ESP_LOGI(TAG, "Purchase session ended");
            }
            break;

        case DOOR_SESSION_IDLE:
        default:
            break;
    }
}

static void door_session_task(void *context) {
    (void)context;

    while (1) {
        if (state_mutex == NULL ||
            xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE) {
            process_door_state();

            if (state_mutex != NULL) {
                xSemaphoreGive(state_mutex);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(DOOR_POLL_INTERVAL_MS));
    }
}
