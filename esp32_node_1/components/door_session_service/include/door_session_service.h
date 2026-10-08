#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DOOR_SESSION_IDLE = 0,
    DOOR_SESSION_WAITING_FOR_OPEN,
    DOOR_SESSION_ACTIVE,
} door_session_state_t;

typedef struct {
    bool door_closed;
    bool lock_unlocked;
    door_session_state_t session_state;
} door_session_status_t;

void door_session_service_init(void);
void door_session_service_start(void);
void door_session_service_request_unlock(void);
void door_session_service_get_status(door_session_status_t *status);
/* Read-only debounced physical boundary; relay timeout never creates one. */
bool door_session_service_get_close_boundary(uint32_t *boundary_tick);
const char *door_session_service_state_name(door_session_state_t state);

#ifdef __cplusplus
}
#endif
