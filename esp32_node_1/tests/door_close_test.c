/* Drive the real production state machine through host GPIO/time/UART stubs. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../components/door_session_service/door_session_service.c"
static TickType_t now;
static bool raw_closed = true, available = true;
static unsigned opens, closes;
static char output[4096], logs[16384];
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
int xSemaphoreTake(SemaphoreHandle_t m, TickType_t t) { (void)m; (void)t; return available; }
int xSemaphoreGive(SemaphoreHandle_t m) { (void)m; return pdTRUE; }
TickType_t xTaskGetTickCount(void) { return now; }
void vTaskDelay(TickType_t t) { now += t; }
int xTaskCreate(void (*f)(void *), const char *n, unsigned s, void *c, unsigned p, void *h) {
    (void)f; (void)n; (void)s; (void)c; (void)p; (void)h; return pdTRUE;
}
void host_log(const char *t, const char *f, ...) {
    (void)t;
    size_t used = strlen(logs);
    va_list args; va_start(args, f);
    int size = vsnprintf(logs + used, sizeof(logs) - used, f, args);
    va_end(args);
    assert(size >= 0 && (size_t)size + used + 1 < sizeof(logs));
    strcat(logs, "\n");
}
void lock_init(void) {} void door_sensor_init(void) {}
void lock_open(void) { ++opens; }
void lock_close(void) { ++closes; }
bool is_door_closed(void) { return raw_closed; }
void send_to_pi(const char *s) { assert(strlen(output) + strlen(s) < sizeof(output)); strcat(output, s); }
static void poll(bool closed, unsigned count) {
    raw_closed = closed;
    for (unsigned i = 0; i < count; ++i) { now += 20; process_door_state(); }
}
static void reset(bool closed) {
    now = 0; raw_closed = closed; available = true; opens = closes = 0;
    output[0] = logs[0] = 0; door_session_service_init();
}
static void status_is(door_session_state_t state, bool closed, bool unlocked) {
    door_session_status_t status;
    door_session_service_get_status(&status);
    assert(status.session_state == state && status.door_closed == closed && status.lock_unlocked == unlocked);
}
static void no_boundary(void) {
    uint32_t boundary = 123;
    assert(!door_session_service_get_close_boundary(&boundary) && boundary == 123);
}
static unsigned occurrences(const char *haystack, const char *needle) {
    unsigned count = 0;
    while ((haystack = strstr(haystack, needle))) { ++count; haystack += strlen(needle); }
    return count;
}
static void start_active(void) {
    reset(true); door_session_service_request_unlock(); poll(false, 3);
    status_is(DOOR_SESSION_ACTIVE, false, true);
}
static void test_boot_closed(void) {
    reset(true); status_is(DOOR_SESSION_IDLE, true, false); no_boundary();
    assert(!opens && !closes && !output[0]);
    assert(strstr(logs, "raw=0 | door=closed | lock=locked | session=idle"));
}
static void test_unlock_waits(void) {
    reset(true); door_session_service_request_unlock(); poll(true, 10);
    status_is(DOOR_SESSION_WAITING_FOR_OPEN, true, true); no_boundary();
    assert(opens == 1 && !closes && !strcmp(output, "LOCK: unlocked\n"));
    assert(strstr(logs, "UNLOCK_REQUEST door=closed session=idle lock=locked relay=off"));
}
static void test_no_open_timeout(void) {
    reset(true); door_session_service_request_unlock(); now = 4999; process_door_state();
    status_is(DOOR_SESSION_WAITING_FOR_OPEN, true, true);
    now = 5000; process_door_state(); now = 6000; process_door_state();
    status_is(DOOR_SESSION_IDLE, true, false); no_boundary();
    assert(closes == 1 && !strcmp(output, "LOCK: unlocked\nLOCK: locked\n"));
    assert(occurrences(logs, "UNLOCK_TIMEOUT ") == 1);
    assert(!strstr(logs, "PHYSICAL_CLOSE_BOUNDARY"));
}
static void test_real_open(void) {
    start_active(); poll(false, 10); no_boundary();
    assert(!strcmp(output, "LOCK: unlocked\nDOOR: opened\nSESSION: started\n"));
    assert(occurrences(logs, "DOOR_TRANSITION ") == 1);
    assert(strstr(logs, "previous_state=closed debounced_state=open session=waiting_for_open"));
}
static void test_long_active(void) {
    start_active(); now += 6000; poll(false, 10);
    status_is(DOOR_SESSION_ACTIVE, false, true); no_boundary();
    assert(closes == 0 && !strstr(output, "LOCK: locked") && !strstr(output, "DOOR: closed"));
    assert(!strstr(output, "SESSION: ended") && !strstr(logs, "UNLOCK_TIMEOUT"));
}
static void test_real_close_after_long_active(void) {
    start_active(); now += 6000; poll(true, 3);
    status_is(DOOR_SESSION_IDLE, true, false);
    uint32_t boundary = 0; assert(door_session_service_get_close_boundary(&boundary) && boundary == now);
    assert(closes == 1 && !strcmp(output,
        "LOCK: unlocked\nDOOR: opened\nSESSION: started\nDOOR: closed\nLOCK: locked\nSESSION: ended\n"));
    uint32_t original = boundary; poll(true, 10);
    assert(door_session_service_get_close_boundary(&boundary) && boundary == original);
    assert(occurrences(output, "DOOR: closed") == 1 && occurrences(output, "SESSION: ended") == 1);
    assert(occurrences(logs, "PHYSICAL_CLOSE_BOUNDARY ") == 1);
    assert(strstr(logs, "SESSION_TRANSITION old=active new=idle reason=physical_close"));
}
static void test_already_open_unlock(void) {
    reset(false); door_session_service_request_unlock(); poll(false, 300);
    status_is(DOOR_SESSION_IDLE, false, false); no_boundary();
    assert(!opens && !closes && !strcmp(output, "ERROR: DOOR_OPEN_BEFORE_UNLOCK\n"));
    assert(strstr(logs, "UNLOCK_REJECTED reason=door_already_open"));
    poll(true, 3); no_boundary(); assert(!strstr(output, "SESSION:"));
}
static void test_idle_open_before_unlock(void) {
    reset(true); poll(false, 3); output[0] = 0; door_session_service_request_unlock();
    status_is(DOOR_SESSION_IDLE, false, false); no_boundary();
    assert(!strcmp(output, "ERROR: DOOR_OPEN_BEFORE_UNLOCK\n"));
    poll(false, 300); assert(!strstr(output, "DOOR:") && !strstr(output, "SESSION:"));
}
static void test_bounce(void) {
    reset(true); door_session_service_request_unlock(); poll(false, 2); poll(true, 2);
    status_is(DOOR_SESSION_WAITING_FOR_OPEN, true, true); no_boundary();
    assert(!strstr(output, "DOOR:") && !strstr(output, "SESSION:"));
    poll(false, 3); output[0] = 0;
    poll(true, 2); poll(false, 1); poll(true, 2); no_boundary();
    status_is(DOOR_SESSION_ACTIVE, false, true); assert(!output[0]);
    poll(true, 1); uint32_t boundary;
    assert(door_session_service_get_close_boundary(&boundary));
    assert(occurrences(output, "DOOR: closed") == 1);
}
static void test_timeout_is_not_close(void) {
    reset(true); door_session_service_request_unlock(); now = 6000; process_door_state();
    poll(false, 3); poll(true, 3); no_boundary();
    assert(closes == 1 && !strstr(output, "SESSION:") && !strstr(logs, "PHYSICAL_CLOSE_BOUNDARY"));
}
static void test_reopen_after_boundary(void) {
    start_active(); poll(true, 3); uint32_t boundary;
    assert(door_session_service_get_close_boundary(&boundary));
    uint32_t original = boundary; poll(false, 3); no_boundary();
    door_session_service_request_unlock(); no_boundary(); poll(true, 3); no_boundary();
    status_is(DOOR_SESSION_IDLE, true, false); assert(close_boundary_at == original);
    assert(occurrences(logs, "PHYSICAL_CLOSE_BOUNDARY ") == 1);
    assert(occurrences(output, "SESSION: started") == 1 && occurrences(output, "SESSION: ended") == 1);
}
static void test_new_unlock_invalidates_old_boundary(void) {
    start_active(); poll(true, 3); uint32_t boundary;
    assert(door_session_service_get_close_boundary(&boundary));
    door_session_service_request_unlock(); no_boundary(); now += 6000; process_door_state(); no_boundary();
    assert(occurrences(logs, "PHYSICAL_CLOSE_BOUNDARY ") == 1);
}
static void test_status_is_read_only(void) {
    start_active(); now += 6000;
    for (unsigned i = 0; i < 10; ++i) { status_is(DOOR_SESSION_ACTIVE, false, true); no_boundary(); }
    assert(!closes && !strstr(output, "DOOR: closed") && !strstr(output, "SESSION: ended"));
}
static void test_duplicate_unlock_does_not_reset_timer(void) {
    reset(true); door_session_service_request_unlock(); now = 4000; door_session_service_request_unlock();
    assert(opens == 1 && unlock_started_at == 0 && strstr(output, "ERROR: UNLOCK_SESSION_NOT_IDLE"));
    now = 5000; process_door_state(); status_is(DOOR_SESSION_IDLE, true, false); no_boundary();
    start_active(); door_session_service_request_unlock(); status_is(DOOR_SESSION_ACTIVE, false, true);
    assert(opens == 1 && !closes && strstr(output, "ERROR: DOOR_OPEN_BEFORE_UNLOCK"));
}
static void test_open_at_deadline(void) {
    reset(true); door_session_service_request_unlock(); now = 4940; poll(false, 3);
    assert(now == 5000); status_is(DOOR_SESSION_ACTIVE, false, true);
    assert(!closes && !strstr(logs, "UNLOCK_TIMEOUT"));
}
static void test_busy_mutex_and_tick_wrap(void) {
    start_active(); poll(true, 3); uint32_t boundary;
    available = false; assert(!door_session_service_get_close_boundary(&boundary)); available = true;
    reset(true); now = UINT32_MAX - 1000; door_session_service_request_unlock();
    now += 4999; process_door_state(); status_is(DOOR_SESSION_WAITING_FOR_OPEN, true, true);
    now += 1; process_door_state(); status_is(DOOR_SESSION_IDLE, true, false); no_boundary();
}
#define RUN(test) do { test(); puts("PASS " #test); } while (0)
int main(void) {
    RUN(test_boot_closed); RUN(test_unlock_waits); RUN(test_no_open_timeout);
    RUN(test_real_open); RUN(test_long_active); RUN(test_real_close_after_long_active);
    RUN(test_already_open_unlock); RUN(test_idle_open_before_unlock); RUN(test_bounce);
    RUN(test_timeout_is_not_close); RUN(test_reopen_after_boundary);
    RUN(test_new_unlock_invalidates_old_boundary); RUN(test_status_is_read_only);
    RUN(test_duplicate_unlock_does_not_reset_timer); RUN(test_open_at_deadline);
    RUN(test_busy_mutex_and_tick_wrap);
    return 0;
}
