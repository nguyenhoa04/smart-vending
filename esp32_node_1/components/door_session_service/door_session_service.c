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
static void transition_session(door_session_state_t next, const char *reason) {
    if (session_state == next) return;
    ESP_LOGI(TAG, "SESSION_TRANSITION old=%s new=%s reason=%s",
             door_session_service_state_name(session_state),
             door_session_service_state_name(next), reason);
    session_state = next;
}

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
        ESP_LOGI(TAG, "UNLOCK_REQUEST door=%s session=%s lock=%s relay=%s",
                 stable_door_closed ? "closed" : "open",
                 door_session_service_state_name(session_state),
                 lock_unlocked ? "unlocked" : "locked", lock_unlocked ? "on" : "off");
        if (!stable_door_closed || session_state != DOOR_SESSION_IDLE) {
            const char *reason = !stable_door_closed ? "door_already_open" : "session_not_idle";
            ESP_LOGW(TAG, "UNLOCK_REJECTED reason=%s door=%s session=%s lock=%s",
                     reason, stable_door_closed ? "closed" : "open",
                     door_session_service_state_name(session_state),
                     lock_unlocked ? "unlocked" : "locked");
            send_to_pi(!stable_door_closed ? "ERROR: DOOR_OPEN_BEFORE_UNLOCK\n" :
                                           "ERROR: UNLOCK_SESSION_NOT_IDLE\n");
            if (state_mutex != NULL) xSemaphoreGive(state_mutex);
            return;
        }
        // A boundary from an earlier session cannot authorize this session's final sample.
        close_boundary_valid = false;
        lock_open();
        lock_unlocked = true;
        unlock_started_at = xTaskGetTickCount();

        transition_session(DOOR_SESSION_WAITING_FOR_OPEN, "unlock_accepted");

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
    bool previous_door_closed = stable_door_closed;
    bool raw_door_closed = is_door_closed();
    bool door_changed = update_debounced_door_state(raw_door_closed);
    if (door_changed) {
        // Only a debounced OPEN -> CLOSED in ACTIVE establishes commerce evidence.
        // Any reopen invalidates final-snapshot eligibility, including after checkout.
        close_boundary_valid = !previous_door_closed && stable_door_closed &&
                               session_state == DOOR_SESSION_ACTIVE;
        if (close_boundary_valid) {
            close_boundary_at = xTaskGetTickCount();
        }
        ESP_LOGI(TAG, "DOOR_TRANSITION GPIO=%d raw=%d previous_state=%s debounced_state=%s session=%s",
                 DOOR_SENSOR_PIN, raw_door_closed ? 0 : 1,
                 previous_door_closed ? "closed" : "open",
                 stable_door_closed ? "closed" : "open",
                 door_session_service_state_name(session_state));
        ESP_LOGI(
            TAG,
            "MC-38 changed | GPIO=%d | raw=%d | door=%s",
            DOOR_SENSOR_PIN,
            stable_door_closed ? 0 : 1,
            stable_door_closed ? "closed" : "open"
        );
        if (close_boundary_valid) {
            ESP_LOGI(TAG, "PHYSICAL_CLOSE_BOUNDARY tick=%lu time_ms=%lu door=closed session=active",
                     (unsigned long)close_boundary_at,
                     (unsigned long)(close_boundary_at * portTICK_PERIOD_MS));
        }
        send_to_pi(stable_door_closed ? "DOOR: closed\n" : "DOOR: opened\n");
    }

    switch (session_state) {
        case DOOR_SESSION_WAITING_FOR_OPEN:
            if (door_changed && previous_door_closed && !stable_door_closed) {
                transition_session(DOOR_SESSION_ACTIVE, "physical_open");
                send_to_pi("SESSION: started\n");
                ESP_LOGI(TAG, "Purchase session started");
            } else if (lock_unlocked &&
                       xTaskGetTickCount() - unlock_started_at >= pdMS_TO_TICKS(UNLOCK_TIMEOUT_MS)) {
                ESP_LOGW(TAG, "UNLOCK_TIMEOUT elapsed_ticks=%lu door=%s session=waiting_for_open",
                         (unsigned long)(xTaskGetTickCount() - unlock_started_at),
                         stable_door_closed ? "closed" : "open");
                close_lock_if_needed();
                transition_session(DOOR_SESSION_IDLE, "no_open_timeout");
            }
            break;

        case DOOR_SESSION_ACTIVE:
            if (door_changed && !previous_door_closed && stable_door_closed) {
                close_lock_if_needed();
                transition_session(DOOR_SESSION_IDLE, "physical_close");
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
