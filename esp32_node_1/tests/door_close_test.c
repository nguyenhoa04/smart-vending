/* Exercise the real debouncer and relay timeout, not a simulated close. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../components/door_session_service/door_session_service.c"
static TickType_t now;
static bool raw_closed = true, available = true;
static unsigned closes;
static char output[4096];
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
int xSemaphoreTake(SemaphoreHandle_t m, TickType_t t) { (void)m; (void)t; return available; }
int xSemaphoreGive(SemaphoreHandle_t m) { (void)m; return pdTRUE; }
TickType_t xTaskGetTickCount(void) { return now; }
void vTaskDelay(TickType_t t) { now += t; }
int xTaskCreate(void (*f)(void *), const char *n, unsigned s, void *c, unsigned p, void *h) {
    (void)f; (void)n; (void)s; (void)c; (void)p; (void)h; return pdTRUE;
}
void host_log(const char *t, const char *f, ...) { (void)t; (void)f; }
void lock_init(void) {} void door_sensor_init(void) {} void lock_open(void) {}
void lock_close(void) { ++closes; }
bool is_door_closed(void) { return raw_closed; }
void send_to_pi(const char *s) { strcat(output, s); }
static void poll(bool closed, unsigned count) {
    raw_closed = closed;
    for (unsigned i=0; i<count; ++i) { now+=20; process_door_state(); }
}
int main(void) {
    uint32_t boundary=0;
    door_session_service_init();
    assert(!door_session_service_get_close_boundary(&boundary));
    door_session_service_request_unlock(); now=6000; process_door_state();
    assert(closes==1 && !strstr(output,"DOOR: closed"));
    assert(!door_session_service_get_close_boundary(&boundary));
    puts("PASS boot closed and relay timeout do not establish commerce close boundary");
    output[0]=0; door_session_service_request_unlock(); poll(false,3);
    assert(strstr(output,"DOOR: opened") && session_state==DOOR_SESSION_ACTIVE);
    output[0]=0; now+=6000; process_door_state();
    assert(strstr(output,"LOCK: locked") && !strstr(output,"DOOR: closed"));
    assert(!door_session_service_get_close_boundary(&boundary));
    poll(true,2); assert(!door_session_service_get_close_boundary(&boundary));
    poll(false,1); poll(true,2); assert(!door_session_service_get_close_boundary(&boundary));
    poll(true,1); assert(door_session_service_get_close_boundary(&boundary));
    assert(boundary==now && strstr(output,"DOOR: closed") && strstr(output,"SESSION: ended"));
    uint32_t original=boundary; poll(true,5);
    assert(door_session_service_get_close_boundary(&boundary) && boundary==original);
    puts("PASS real three-sample debounced close establishes exactly one immutable boundary");
    available=false; assert(!door_session_service_get_close_boundary(&boundary)); available=true;
    poll(false,3); assert(!door_session_service_get_close_boundary(&boundary));
    puts("PASS busy mutex and reopened door cannot prove a safe final sample");
    return 0;
}
