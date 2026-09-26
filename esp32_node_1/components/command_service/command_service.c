#include "command_service.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "door_session_service.h"
#include "pi_uart.h"
#include "shelf_service.h"

#define COMMAND_BUFFER_SIZE 128

static void process_command(const char *command);
static void normalize_command(const char *source, char *destination, size_t size);
static bool parse_shelf_id(const char *text, uint8_t *shelf_id);
static void process_tare_command(const char *command);
static void process_calibrate_command(const char *command);
static void console_task(void *context);
static void uart_task(void *context);

void command_service_start(void) {
    xTaskCreate(uart_task, "LOCK", 4096, NULL, 5, NULL);
    xTaskCreate(console_task, "CONSOLE", 4096, NULL, 5, NULL);
}

static void process_command(const char *command) {
    char normalized[COMMAND_BUFFER_SIZE];
    normalize_command(command, normalized, sizeof(normalized));

    if (strcmp(normalized, "UNLOCK") == 0) {
        ESP_LOGI("LOCK", "UNLOCK command received");
        door_session_service_request_unlock();
    } else if (strcmp(normalized, "STATUS") == 0) {
        door_session_status_t door_status = {0};
        door_session_service_get_status(&door_status);

        char status_buffer[COMMAND_BUFFER_SIZE];
        snprintf(
            status_buffer,
            sizeof(status_buffer),
            "STATUS: door=%s;lock=%s;session=%s;weight=%.1f\n",
            door_status.door_closed ? "closed" : "open",
            door_status.lock_unlocked ? "unlocked" : "locked",
            door_session_service_state_name(door_status.session_state),
            shelf_service_get_primary_total()
        );
        send_to_pi(status_buffer);
    } else if (strncmp(normalized, "TARE", strlen("TARE")) == 0) {
        process_tare_command(normalized);
    } else if (strncmp(normalized, "CALIBRATE:", strlen("CALIBRATE:")) == 0) {
        process_calibrate_command(normalized);
    } else if (strcmp(normalized, "CORNER1") == 0) {
        shelf_service_record_corner(1);
    } else if (strcmp(normalized, "CORNER2") == 0) {
        shelf_service_record_corner(2);
    } else if (strcmp(normalized, "CORNER3") == 0) {
        shelf_service_record_corner(3);
    } else if (strcmp(normalized, "CORNER4") == 0) {
        shelf_service_record_corner(4);
    } else if (strcmp(normalized, "CORNERCHECK") == 0) {
        shelf_service_report_corner_deviation();
    } else if (strcmp(normalized, "PING") == 0) {
        send_to_pi("READY\n");
    }
}

static void normalize_command(const char *source, char *destination, size_t size) {
    if (source == NULL || destination == NULL || size == 0) {
        return;
    }

    while (*source == ' ' || *source == '\t' ||
           *source == '\r' || *source == '\n') {
        ++source;
    }

    size_t length = strnlen(source, size - 1);
    while (length > 0 &&
           (source[length - 1] == ' ' || source[length - 1] == '\t' ||
            source[length - 1] == '\r' || source[length - 1] == '\n')) {
        --length;
    }

    memcpy(destination, source, length);
    destination[length] = '\0';
}

static bool parse_shelf_id(const char *text, uint8_t *shelf_id) {
    static const char prefix[] = "SHELF_";
    if (text == NULL || shelf_id == NULL ||
        strncmp(text, prefix, strlen(prefix)) != 0) {
        return false;
    }

    char *end = NULL;
    long parsed_id = strtol(text + strlen(prefix), &end, 10);
    if (end == text + strlen(prefix) || *end != '\0' ||
        parsed_id < 1 || parsed_id > UINT8_MAX) {
        return false;
    }

    *shelf_id = (uint8_t)parsed_id;
    return true;
}

static void process_tare_command(const char *command) {
    uint8_t shelf_id = 1;

    if (strcmp(command, "TARE") != 0) {
        static const char prefix[] = "TARE:";
        if (strncmp(command, prefix, strlen(prefix)) != 0 ||
            !parse_shelf_id(command + strlen(prefix), &shelf_id)) {
            send_to_pi("TARE: ERROR format; use TARE:SHELF_n\n");
            return;
        }
    }

    char response[64];
    if (shelf_service_tare(shelf_id)) {
        snprintf(response, sizeof(response), "TARE: SHELF_%u OK\n", shelf_id);
    } else {
        snprintf(
            response,
            sizeof(response),
            "TARE: SHELF_%u ERROR inactive or no samples\n",
            shelf_id
        );
    }
    send_to_pi(response);
}

static void process_calibrate_command(const char *command) {
    static const char prefix[] = "CALIBRATE:";
    const char *arguments = command + strlen(prefix);
    uint8_t shelf_id = 1;
    const char *weight_text = arguments;

    if (strncmp(arguments, "SHELF_", strlen("SHELF_")) == 0) {
        const char *separator = strchr(arguments, ':');
        if (separator == NULL) {
            send_to_pi(
                "CALIBRATE: ERROR format; use CALIBRATE:SHELF_n:grams\n"
            );
            return;
        }

        char shelf_text[16];
        size_t shelf_text_length = (size_t)(separator - arguments);
        if (shelf_text_length >= sizeof(shelf_text)) {
            send_to_pi("CALIBRATE: ERROR invalid shelf\n");
            return;
        }
        memcpy(shelf_text, arguments, shelf_text_length);
        shelf_text[shelf_text_length] = '\0';
        if (!parse_shelf_id(shelf_text, &shelf_id)) {
            send_to_pi("CALIBRATE: ERROR invalid shelf\n");
            return;
        }
        weight_text = separator + 1;
    }

    char *end = NULL;
    float known_weight = strtof(weight_text, &end);
    if (end == weight_text || *end != '\0' || known_weight <= 0.0f) {
        send_to_pi("CALIBRATE: ERROR invalid weight\n");
        return;
    }

    shelf_calibration_result_t result = {0};
    char response[128];
    if (shelf_service_calibrate(shelf_id, known_weight, &result)) {
        snprintf(
            response,
            sizeof(response),
            "CALIBRATE: SHELF_%u OK weight=%.1fg scale=%.4f raw_avg=%ld\n",
            shelf_id,
            known_weight,
            result.scale,
            (long)result.raw_average
        );
    } else {
        snprintf(
            response,
            sizeof(response),
            "CALIBRATE: SHELF_%u ERROR inactive, no samples or scale too small\n",
            shelf_id
        );
    }
    send_to_pi(response);
}

static void console_task(void *context) {
    (void)context;
    char line[COMMAND_BUFFER_SIZE];
    int position = 0;

    while (1) {
        int character = fgetc(stdin);
        if (character != EOF) {
            if (character == '\n' || character == '\r') {
                if (position > 0) {
                    line[position] = '\0';
                    process_command(line);
                    position = 0;
                }
            } else if (position < (int)sizeof(line) - 1) {
                line[position++] = (char)character;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

static void uart_task(void *context) {
    (void)context;
    uint8_t receive_buffer[COMMAND_BUFFER_SIZE];

    while (1) {
        int32_t length = uart_read_bytes(
            UART_PORT_NUM,
            receive_buffer,
            sizeof(receive_buffer) - 1,
            pdMS_TO_TICKS(20)
        );
        if (length > 0) {
            receive_buffer[length] = '\0';
            process_command((const char *)receive_buffer);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
