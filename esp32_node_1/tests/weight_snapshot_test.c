/* Execute production command dispatch and shelf service against host I/O stubs. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../components/shelf_service/shelf_service.c"
#include "../components/command_service/command_service.c"

static TickType_t host_now = 5000;
static bool lock_available = true;
static bool lock_held;
static unsigned raw_reads, delays, unlocks, gives;
static char output[4096];

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t ticks) {
    assert(mutex != NULL && ticks == pdMS_TO_TICKS(50));
    lock_held = lock_available;
    return lock_available ? pdTRUE : 0;
}
int xSemaphoreGive(SemaphoreHandle_t mutex) {
    assert(mutex != NULL && lock_held);
    lock_held = false;
    ++gives;
    return pdTRUE;
}
TickType_t xTaskGetTickCount(void) { return host_now; }
void vTaskDelay(TickType_t ticks) { (void)ticks; ++delays; }
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack,
                void *context, unsigned priority, void *handle) {
    (void)task; (void)name; (void)stack; (void)context; (void)priority; (void)handle;
    return pdTRUE;
}
void host_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
void send_to_pi(const char *data) {
    assert(!lock_held);  /* Snapshot TX must not hold the sensor mutex. */
    assert(strlen(output) + strlen(data) < sizeof(output));
    strcat(output, data);
}
int uart_read_bytes(int port, void *data, uint32_t size, uint32_t timeout) {
    (void)port; (void)data; (void)size; (void)timeout;
    return 0;
}
bool hx711_init(hx711_config_t *config) { (void)config; return true; }
bool hx711_read_raw_timeout(const hx711_config_t *config, int32_t *value, uint32_t timeout) {
    (void)config; (void)value; (void)timeout; ++raw_reads;
    return false;
}
float hx711_calibrate_raw(const hx711_config_t *config, int32_t value) {
    return (value - config->offset) / config->scale;
}
void door_session_service_request_unlock(void) { ++unlocks; }
void door_session_service_get_status(door_session_status_t *status) {
    *status = (door_session_status_t){.door_closed = true, .session_state = DOOR_SESSION_IDLE};
}
const char *door_session_service_state_name(door_session_state_t state) { (void)state; return "idle"; }

static void clear_output(void) { memset(output, 0, sizeof(output)); }

int main(void) {
    shelf_mutex = xSemaphoreCreateMutex();
    for (uint8_t i = 0; i < SHELF_COUNT; ++i) {
        shelves[i].sample_count = MOVING_AVERAGE_WINDOW;
        shelves[i].sample_index = 7;
        shelves[i].raw_value = 12345 + i;
        shelves[i].total_weight = i == 0 ? 1050.0f : 239.0f;
        shelves[i].last_sample_at = host_now - 100;
        for (uint8_t j = 0; j < MOVING_AVERAGE_WINDOW; ++j) shelves[i].samples[j] = 42.0f + j;
    }
    shelf_runtime_t before[SHELF_COUNT];
    memcpy(before, shelves, sizeof(shelves));
    process_command("  WEIGHT_SNAPSHOT\r\n");
    assert(strcmp(output, "SHELF_1: TOTAL=1050.0\nSHELF_2: TOTAL=239.0\n") == 0);
    assert(memcmp(before, shelves, sizeof(shelves)) == 0);
    assert(raw_reads == 0 && delays == 0 && unlocks == 0 && gives == 1);
    puts("PASS snapshot command publishes both shelves without tare/calibration/state mutation");

    clear_output();
    process_command("WEIGHT_SNAPSHOT_BAD");
    assert(output[0] == '\0');
    puts("PASS command matching rejects suffixes");

    shelves[0].last_sample_at = host_now - 1001;
    shelves[1].sample_count = 0;
    process_command("WEIGHT_SNAPSHOT");
    assert(output[0] == '\0');
    shelves[0].last_sample_at = host_now;
    shelves[0].total_weight = NAN;
    process_command("WEIGHT_SNAPSHOT");
    assert(output[0] == '\0');
    memcpy(shelves, before, sizeof(shelves));
    puts("PASS stale/uninitialized/nonfinite sensor values are not published");

    lock_available = false;
    process_command("WEIGHT_SNAPSHOT");
    assert(output[0] == '\0');
    lock_available = true;
    shelf_mutex = NULL;
    process_command("WEIGHT_SNAPSHOT");
    assert(output[0] == '\0');
    shelf_mutex = xSemaphoreCreateMutex();
    puts("PASS busy or missing mutex safely skips publication");

    process_uart_bytes((const uint8_t *)"WEIGHT_", 7);
    assert(output[0] == '\0');
    const char *tail = "SNAPSHOT\r\nPING\n";
    process_uart_bytes((const uint8_t *)tail, strlen(tail));
    assert(strcmp(output, "SHELF_1: TOTAL=1050.0\nSHELF_2: TOTAL=239.0\nREADY\n") == 0);
    assert(memcmp(before, shelves, sizeof(shelves)) == 0);
    puts("PASS split/coalesced UART commands retain FIFO dispatch");

    clear_output();
    uint8_t oversized[COMMAND_BUFFER_SIZE + 20];
    memset(oversized, 'X', sizeof(oversized));
    process_uart_bytes(oversized, sizeof(oversized));
    const char *next = "WEIGHT_SNAPSHOT\nWEIGHT_SNAPSHOT\n";
    process_uart_bytes((const uint8_t *)next, strlen(next));
    assert(strcmp(output, "SHELF_1: TOTAL=1050.0\nSHELF_2: TOTAL=239.0\n") == 0);
    puts("PASS oversized UART line is discarded and next command recovers");

    clear_output();
    host_now = 5;
    shelves[0].last_sample_at = shelves[1].last_sample_at = UINT32_MAX - 20;
    process_command("WEIGHT_SNAPSHOT");
    assert(strcmp(output, "SHELF_1: TOTAL=1050.0\nSHELF_2: TOTAL=239.0\n") == 0);
    clear_output();
    process_command("STATUS");
    assert(strcmp(output, "STATUS: door=closed;lock=locked;session=idle;weight=1050.0\n") == 0);
    assert(raw_reads == 0 && delays == 0 && unlocks == 0);
    puts("PASS freshness handles tick rollover; STATUS keeps primary-shelf semantics");
    return 0;
}
