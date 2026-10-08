/* Production shelf/command/node startup + real calibration_store, fake hardware/NVS.
 * Each scenario runs in a fresh process; simulated restarts retain only NVS data.
 */
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "node_app.h"
#include "../components/shelf_service/shelf_service.c"
#include "../components/command_service/command_service.c"

/* Existing v1 on-device layout, seeded without calling the new runtime. */
typedef struct {
    uint32_t magic;
    uint16_t version, size;
    int32_t offset;
    float scale;
    uint8_t flags, reserved[3];
} legacy_record_t;
_Static_assert(sizeof(legacy_record_t) == 20, "NVS v1 layout must remain compatible");
static legacy_record_t stored[2], pending;
static size_t stored_size[2];
static unsigned pending_shelf;
static bool pending_valid, uart_ready, lock_held, sensor_running, raw_ready = true;
static unsigned nvs_inits, nvs_reads, nvs_writes, nvs_commits, nvs_erases, hx_inits;
static unsigned raw_reads[2], sensor_iterations;
static esp_err_t init_error = ESP_OK, write_error = ESP_OK, commit_error = ESP_OK;
static int32_t input_raw[2] = {211000, -64750};  /* 1050g, 239g on occupied shelves. */
static TickType_t host_now;
static char output[8192], logs[32768];
static void (*sensor_entry)(void *);
static jmp_buf sensor_finished;

static void seed_valid(void) {
    stored[0] = (legacy_record_t){0x43414C31, 1, 20, 1000, 200.0f, 3, {0}};
    stored[1] = (legacy_record_t){0x43414C31, 1, 20, -5000, -250.0f, 3, {0}};
    stored_size[0] = stored_size[1] = 20;
}
static unsigned key_shelf(const char *key) {
    assert(strcmp(key, "shelf_1") == 0 || strcmp(key, "shelf_2") == 0);
    return key[6] - '1';
}
const char *esp_err_to_name(esp_err_t error) {
    switch (error) {
        case ESP_OK: return "ESP_OK";
        case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
        case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
        case ESP_ERR_INVALID_VERSION: return "ESP_ERR_INVALID_VERSION";
        case ESP_ERR_NVS_NO_FREE_PAGES: return "ESP_ERR_NVS_NO_FREE_PAGES";
        case ESP_ERR_NVS_NEW_VERSION_FOUND: return "ESP_ERR_NVS_NEW_VERSION_FOUND";
        default: return "ESP_FAIL";
    }
}
esp_err_t nvs_flash_init(void) { assert(uart_ready); ++nvs_inits; return init_error; }
esp_err_t nvs_flash_erase(void) { ++nvs_erases; assert(!"NVS must never be erased"); return ESP_FAIL; }
esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle) {
    assert(strcmp(name, "shelf_cal") == 0);
    *handle = mode == NVS_READONLY ? 1 : 2;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *size) {
    assert(handle == 1);
    unsigned id = key_shelf(key);
    ++nvs_reads;
    if (!stored_size[id]) return ESP_ERR_NVS_NOT_FOUND;
    assert(*size >= stored_size[id]);
    memcpy(value, &stored[id], stored_size[id]);
    *size = stored_size[id];
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size) {
    assert(handle == 2 && size == 20);
    ++nvs_writes;
    if (write_error != ESP_OK) return write_error;
    pending_shelf = key_shelf(key);
    memcpy(&pending, value, size);
    assert(pending.magic == 0x43414C31 && pending.version == 1 && pending.size == 20);
    pending_valid = true;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) {
    assert(handle == 2 && pending_valid);
    ++nvs_commits;
    if (commit_error != ESP_OK) return commit_error;
    stored[pending_shelf] = pending;
    stored_size[pending_shelf] = 20;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { (void)handle; pending_valid = false; }
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key) {
    (void)handle; (void)key; ++nvs_erases; assert(!"Runtime must not reset calibration"); return ESP_FAIL;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t ticks) {
    assert(mutex && !lock_held && (ticks == pdMS_TO_TICKS(50) || ticks == portMAX_DELAY));
    lock_held = true;
    return pdTRUE;
}
int xSemaphoreGive(SemaphoreHandle_t mutex) {
    assert(mutex && lock_held); lock_held = false; return pdTRUE;
}
TickType_t xTaskGetTickCount(void) { return host_now; }
void vTaskDelay(TickType_t ticks) {
    host_now += ticks;
    if (sensor_running && ticks == pdMS_TO_TICKS(20) && ++sensor_iterations == 20) {
        assert(!lock_held);
        longjmp(sensor_finished, 1);
    }
}
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack,
                void *context, unsigned priority, void *handle) {
    (void)stack; (void)context; (void)priority; (void)handle;
    assert(hx_inits >= 2 && nvs_inits == 1 && !lock_held);
    if (strcmp(name, "main_sensor_task") == 0) sensor_entry = task;
    return pdTRUE;
}
void host_log(const char *tag, const char *format, ...) {
    (void)tag;
    size_t position = strlen(logs);
    va_list args;
    va_start(args, format);
    int count = vsnprintf(logs + position, sizeof(logs) - position, format, args);
    va_end(args);
    assert(count >= 0 && position + count + 1 < sizeof(logs));
    strcat(logs, "\n");
}
void init_uart(void) { uart_ready = true; }
void send_to_pi(const char *data) {
    if (!sensor_running) assert(!lock_held);  /* Snapshot TX is outside mutex. */
    assert(strlen(output) + strlen(data) < sizeof(output));
    strcat(output, data);
}
int uart_read_bytes(int port, void *data, uint32_t size, uint32_t timeout) {
    (void)port; (void)data; (void)size; (void)timeout; return 0;
}
bool hx711_init(hx711_config_t *config) {
    assert(nvs_inits == 1 && uart_ready);
    if (init_error == ESP_OK) assert(nvs_reads >= 2);  /* Both loaded before HX initialization. */
    shelf_runtime_t *shelf = &shelves[config->id - 1];
    assert(config->offset == (shelf->calibration.offset_valid ? shelf->calibration.offset : 0));
    assert(config->scale == (shelf->calibration.scale_valid ? shelf->calibration.scale : 0));
    ++hx_inits;
    return true;
}
bool hx711_read_raw_timeout(const hx711_config_t *config, int32_t *value, uint32_t timeout) {
    assert(timeout == HX711_READY_TIMEOUT_MS);
    ++raw_reads[config->id - 1];
    *value = input_raw[config->id - 1];
    return raw_ready;
}
float hx711_calibrate_raw(const hx711_config_t *config, int32_t value) {
    assert(shelf_calibration_valid(&shelves[config->id - 1]));
    return (value - config->offset) / config->scale;
}
void door_session_service_init(void) { assert(hx_inits >= 2 && nvs_inits == 1); }
void door_session_service_start(void) {}
bool door_session_service_get_close_boundary(uint32_t *tick) { (void)tick; return false; }
void door_session_service_request_unlock(void) { assert(!"Snapshot must not unlock"); }
void door_session_service_get_status(door_session_status_t *status) {
    *status = (door_session_status_t){.door_closed = true, .session_state = DOOR_SESSION_IDLE};
}
const char *door_session_service_state_name(door_session_state_t state) { (void)state; return "idle"; }

static void boot(void) {
    unsigned reads[2] = {raw_reads[0], raw_reads[1]}, writes = nvs_writes, commits = nvs_commits;
    memset(logs, 0, sizeof(logs));
    output[0] = '\0';
    for (unsigned i = 0; i < 2; ++i) {
        /* Poison previous RAM: next startup must use the persisted blob. */
        shelves[i].hx711.offset = 999;
        shelves[i].hx711.scale = 777;
        shelves[i].calibration = (calibration_store_data_t){0};
    }
    node_app_start();
    assert(nvs_inits == 1 && nvs_erases == 0);
    assert(raw_reads[0] - reads[0] == 10 && raw_reads[1] - reads[1] == 10);
    assert(nvs_writes == writes && nvs_commits == commits && output[0] == '\0');
    assert(strstr(logs, "Startup auto-tare disabled, using persisted calibration only"));
    assert(!strstr(logs, "Auto-taring") && !strstr(logs, "tare complete"));
    assert(shelves[0].sample_count == 0 && shelves[1].sample_count == 0);
    assert(isnan(shelf_service_get_primary_total()));
}
static void measure_loaded_shelves(void) {
    sensor_iterations = 0;
    sensor_running = true;
    if (setjmp(sensor_finished) == 0) sensor_entry(NULL);
    sensor_running = false;
    assert(sensor_iterations == 20);
}
static void snapshots_are_read_only(const char *expected) {
    shelf_runtime_t before[2];
    memcpy(before, shelves, sizeof(shelves));
    unsigned reads = raw_reads[0] + raw_reads[1];
    unsigned calls = nvs_reads + nvs_writes + nvs_commits;
    for (unsigned i = 0; i < 5; ++i) {
        output[0] = '\0';
        process_uart_bytes((const uint8_t *)"WEIGHT_SNAPSHOT\n", 16);
        assert(strcmp(output, expected) == 0);
        assert(memcmp(before, shelves, sizeof(shelves)) == 0);
    }
    assert(raw_reads[0] + raw_reads[1] == reads);
    assert(nvs_reads + nvs_writes + nvs_commits == calls && nvs_erases == 0);
}
static void valid_case(void) {
    seed_valid(); boot();
    assert(shelves[0].hx711.offset == 1000 && shelves[0].hx711.scale == 200);
    assert(shelves[1].hx711.offset == -5000 && shelves[1].hx711.scale == -250);
    assert(strstr(logs, "SHELF_1 calibration loaded | offset=1000 | scale=200.0000 | valid=yes"));
    assert(strstr(logs, "SHELF_2 calibration loaded | offset=-5000 | scale=-250.0000 | valid=yes"));
    puts("PASS legacy NVS values/flags load for both shelves before HX711; startup never tares/writes");
    measure_loaded_shelves();
    assert(strcmp(output, "SHELF_1: TOTAL=1050.0\nSHELF_2: TOTAL=239.0\n") == 0);
    snapshots_are_read_only("SHELF_1: TOTAL=1050.0\nSHELF_2: TOTAL=239.0\n");
    puts("PASS occupied shelves publish correct nonzero weights and repeated read-only UART snapshots");
    boot(); measure_loaded_shelves();
    snapshots_are_read_only("SHELF_1: TOTAL=1050.0\nSHELF_2: TOTAL=239.0\n");
    puts("PASS simulated restart reloads identical calibration without physical shelf interaction");
}
static void missing_case(void) {
    boot(); measure_loaded_shelves();
    assert(output[0] == '\0' && shelves[0].hx711.scale == 0 && shelves[1].hx711.scale == 0);
    assert(strstr(logs, "SHELF_1 calibration required") && strstr(logs, "SHELF_2 calibration required"));
    assert(isnan(shelf_service_get_primary_total()));
    snapshots_are_read_only("");
    process_command("STATUS"); assert(strstr(output, "weight=nan"));
    assert(!shelf_service_calibrate(1, 100, NULL) && nvs_writes == 0);
    puts("PASS missing calibration suppresses fake weights; warning/STATUS signal calibration required");
    input_raw[0] = 2000;
    output[0] = '\0'; process_command("TARE");
    assert(strcmp(output, "TARE: SHELF_1 OK\n") == 0);
    assert(stored[0].offset == 2000 && stored[0].flags == 1 && stored[0].scale == 0);
    assert(!shelf_calibration_valid(&shelves[0]));
    input_raw[0] = 22000;
    output[0] = '\0'; process_command("CALIBRATE:100");
    assert(strstr(output, "CALIBRATE: SHELF_1 OK") && stored[0].scale == 200 && stored[0].flags == 3);
    assert(stored[0].offset == 2000 && !stored_size[1]);
    boot(); measure_loaded_shelves();
    snapshots_are_read_only("SHELF_1: TOTAL=100.0\n");
    puts("PASS explicit empty-shelf TARE then known-weight CALIBRATE recover and survive restart");
}
static void partial_case(bool offset_valid) {
    seed_valid(); stored[0].flags = offset_valid ? 1 : 2;
    boot(); measure_loaded_shelves();
    snapshots_are_read_only("SHELF_2: TOTAL=239.0\n");
    assert(!shelf_calibration_valid(&shelves[0]));
    if (offset_valid) {
        assert(shelves[0].hx711.offset == 1000 && shelves[0].hx711.scale == 0);
        assert(shelf_service_calibrate(1, 1050, NULL));
        assert(stored[0].offset == 1000 && stored[0].scale == 200);
    } else {
        assert(shelves[0].hx711.offset == 0 && shelves[0].hx711.scale == 200);
        assert(!shelf_service_calibrate(1, 1050, NULL));
        input_raw[0] = 2000;
        assert(shelf_service_tare(1));
        assert(stored[0].offset == 2000 && stored[0].scale == 200);
    }
    assert(stored[0].flags == 3 && shelf_calibration_valid(&shelves[0]));
    boot(); assert(shelf_calibration_valid(&shelves[0]));
    puts("PASS partial validity flags preserved; only explicit missing-calibration command enables weights");
}
static void persistence_case(void) {
    seed_valid(); boot();
    legacy_record_t other = stored[1];
    input_raw[0] = 2000;
    process_command("TARE");
    assert(strcmp(output, "TARE: SHELF_1 OK\n") == 0);
    assert(stored[0].offset == 2000 && stored[0].scale == 200 && stored[0].flags == 3);
    assert(memcmp(&other, &stored[1], sizeof(other)) == 0);
    input_raw[0] = 32000;
    output[0] = '\0'; process_command("CALIBRATE:100");
    assert(strstr(output, "CALIBRATE: SHELF_1 OK") && stored[0].scale == 300 && stored[0].offset == 2000);
    puts("PASS primary TARE persists only offset; CALIBRATE persists only scale after commit");
    other = stored[0]; input_raw[1] = -6000;
    output[0] = '\0'; process_command("TARE:SHELF_2");
    assert(strcmp(output, "TARE: SHELF_2 OK\n") == 0 && stored[1].scale == -250);
    input_raw[1] = -46000;
    output[0] = '\0'; process_command("CALIBRATE:SHELF_2:100");
    assert(strstr(output, "CALIBRATE: SHELF_2 OK") && stored[1].scale == -400 && stored[1].offset == -6000);
    assert(memcmp(&other, &stored[0], sizeof(other)) == 0 && nvs_commits == 4);
    boot(); measure_loaded_shelves();
    snapshots_are_read_only("SHELF_1: TOTAL=100.0\nSHELF_2: TOTAL=100.0\n");
    puts("PASS shelf-specific commands preserve other shelf and reload both updated values on restart");
}
static void failure_case(void) {
    seed_valid(); boot(); measure_loaded_shelves();
    shelf_runtime_t before[2]; memcpy(before, shelves, sizeof(shelves));
    input_raw[0] = 2000;
    for (unsigned i = 0; i < 2; ++i) {
        write_error = i == 0 ? ESP_FAIL : ESP_OK;
        commit_error = i == 1 ? ESP_FAIL : ESP_OK;
        output[0] = '\0'; process_command("TARE");
        assert(strstr(output, "ERROR") && !strstr(output, " OK"));
        output[0] = '\0'; process_command("CALIBRATE:100");
        assert(strstr(output, "ERROR") && !strstr(output, " OK"));
        assert(memcmp(before, shelves, sizeof(shelves)) == 0);
    }
    write_error = commit_error = ESP_OK;
    raw_ready = false;
    assert(!shelf_service_tare(1) && !shelf_service_calibrate(1, 100, NULL));
    assert(memcmp(before, shelves, sizeof(shelves)) == 0);
    puts("PASS NVS write/commit and HX711 sample failures return ERROR without applying/resetting RAM");
    raw_ready = true;
    unsigned writes = nvs_writes, reads = raw_reads[0];
    const char *invalid[] = {"CALIBRATE:nan", "CALIBRATE:inf", "CALIBRATE:0", "CALIBRATE:-100"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        output[0] = '\0'; process_command(invalid[i]); assert(strstr(output, "ERROR invalid weight"));
    }
    assert(raw_reads[0] == reads);
    assert(!shelf_service_calibrate(1, NAN, NULL) && !shelf_service_calibrate(1, INFINITY, NULL));
    input_raw[0] = shelves[0].hx711.offset;
    assert(!shelf_service_calibrate(1, 100, NULL));
    input_raw[0] += 49;
    assert(!shelf_service_calibrate(1, 100, NULL));
    assert(nvs_writes == writes && memcmp(before, shelves, sizeof(shelves)) == 0);
    puts("PASS nonfinite/nonpositive weights and invalid tiny scales rejected without persistence");
}
static void corrupt_case(void) {
    for (unsigned i = 0; i < 8; ++i) {
        seed_valid();
        switch (i) {
            case 0: stored[0].magic = 0; break;
            case 1: stored[0].version = 2; break;
            case 2: stored[0].size = 0; break;
            case 3: stored[0].scale = NAN; break;
            case 4: stored[0].scale = INFINITY; break;
            case 5: stored[0].scale = 0; break;
            case 6: stored[0].scale = 0.49f; break;
            case 7: stored_size[0] = 19; break;
        }
        boot(); measure_loaded_shelves();
        assert(!shelf_calibration_valid(&shelves[0]) && shelves[0].hx711.scale == 0);
        assert(strstr(logs, "Invalid calibration record for SHELF_1"));
        snapshots_are_read_only("SHELF_2: TOTAL=239.0\n");
    }
    assert(nvs_writes == 0);
    puts("PASS corrupt/mismatched legacy blobs and unsafe scales remain invalid without erasing/replacing NVS");
}
static void init_failure_case(esp_err_t error) {
    seed_valid(); init_error = error;
    legacy_record_t before[2]; memcpy(before, stored, sizeof(stored));
    boot(); measure_loaded_shelves();
    snapshots_are_read_only("");
    assert(strstr(logs, "preserving NVS, calibration requires maintenance"));
    assert(!shelf_service_tare(1) && !shelf_service_calibrate(1, 100, NULL));
    assert(nvs_reads == 0 && nvs_writes == 0 && nvs_erases == 0);
    assert(memcmp(before, stored, sizeof(stored)) == 0);
    puts("PASS NVS initialization failure preserves records; no automatic erase or false successful calibration");
}
int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "valid")) valid_case();
    else if (!strcmp(argv[1], "missing")) missing_case();
    else if (!strcmp(argv[1], "offset-only")) partial_case(true);
    else if (!strcmp(argv[1], "scale-only")) partial_case(false);
    else if (!strcmp(argv[1], "persistence")) persistence_case();
    else if (!strcmp(argv[1], "failure")) failure_case();
    else if (!strcmp(argv[1], "corrupt")) corrupt_case();
    else if (!strcmp(argv[1], "no-free-pages")) init_failure_case(ESP_ERR_NVS_NO_FREE_PAGES);
    else if (!strcmp(argv[1], "new-version")) init_failure_case(ESP_ERR_NVS_NEW_VERSION_FOUND);
    else assert(!"Unknown scenario");
    return 0;
}
