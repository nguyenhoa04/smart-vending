# Physical MC-38 door and relay lifecycle

The MC-38 on GPIO5 is authoritative: low is CLOSED, high is OPEN. Polling remains
20 ms, with three consecutive samples required to change the debounced state.
Calibration/NVS, shelf sampling and HX711 behavior are unchanged.

Previously WAITING_FOR_OPEN entered ACTIVE from an already-stable OPEN level and
both WAITING_FOR_OPEN and ACTIVE applied the original 5000 ms relay timeout. This
could start shopping immediately on UNLOCK and re-lock during shopping.

The production state machine now follows:

```text
IDLE + CLOSED + UNLOCK -> relay ON / LOCK: unlocked -> WAITING_FOR_OPEN
WAITING_FOR_OPEN + real CLOSED -> OPEN -> DOOR: opened -> ACTIVE / SESSION: started
WAITING_FOR_OPEN + 5000 ms without open -> relay OFF / LOCK: locked -> IDLE
ACTIVE + any elapsed time + OPEN -> remain ACTIVE / relay ON
ACTIVE + real OPEN -> CLOSED -> record boundary -> DOOR: closed
                             -> relay OFF / LOCK: locked -> IDLE / SESSION: ended
```

An open transition that finishes debouncing on the same poll as the timeout wins;
it starts ACTIVE and the no-open timeout no longer applies.

UNLOCK while debounced OPEN emits `ERROR: DOOR_OPEN_BEFORE_UNLOCK`, logs
`UNLOCK_REJECTED reason=door_already_open`, and preserves the state/relay. A request
while non-IDLE and CLOSED emits `ERROR: UNLOCK_SESSION_NOT_IDLE`. Duplicate requests
cannot restart the no-open timer or create another session. No synthetic door or
session event is sent by rejection or timeout.

Only a real debounced OPEN -> CLOSED while ACTIVE records `close_boundary_at`.
Stable polling and STATUS never rewrite the tick. `get_close_boundary()` requires
the boundary to remain valid, the door to be CLOSED and the lock to be locked.
Reopening invalidates eligibility; a later idle close cannot recreate it. The old
tick remains diagnostic data. An accepted new unlock also invalidates the old
boundary, so a subsequent no-open timeout cannot reuse earlier checkout evidence.

UART ordering in a normal session is exactly:

```text
LOCK: unlocked
DOOR: opened
SESSION: started
DOOR: closed
LOCK: locked
SESSION: ended
```

INFO/WARNING logs include UNLOCK_REQUEST with door/session/lock/relay,
DOOR_TRANSITION with raw/previous/debounced/session, SESSION_TRANSITION with reason,
UNLOCK_TIMEOUT only in WAITING_FOR_OPEN, UNLOCK_REJECTED and PHYSICAL_CLOSE_BOUNDARY
with tick/time. Unchanged states produce no periodic log traffic.

From this repository's root:

```bash
python3 esp32_node_1/tests/run_door_close_tests.py
python3 esp32_node_1/tests/run_weight_snapshot_tests.py
python3 esp32_node_1/tests/run_calibration_runtime_tests.py
```

The door harness includes production `door_session_service.c`, drives GPIO and
monotonic ticks, and captures real UART/log output. Sixteen cases cover boot,
waiting, timeout, open, shopping beyond five seconds, close and exact UART order,
already-open rejection, bounce, relay-only timeout, reopen, old-boundary reuse,
read-only STATUS, duplicate unlocks, deadline priority, mutex failure and tick wrap.

Build and flash with the existing ESP-IDF installation; replace the serial port:

```bash
source /path/to/esp-idf/export.sh
cd esp32_node_1
cp sdkconfig /tmp/door-lifecycle-sdkconfig
idf.py -B /tmp/door-lifecycle-build -D SDKCONFIG=/tmp/door-lifecycle-sdkconfig build
idf.py -B /tmp/door-lifecycle-build -D SDKCONFIG=/tmp/door-lifecycle-sdkconfig \
  -p /dev/ttyUSB0 flash monitor
```

Ordinary flash writes the bootloader/partition table/app and preserves the NVS
partition. Do not erase flash, erase NVS or run TARE/CALIBRATE as part of this fix.
Occasional `SHELF_n HX711 timeout` warnings are a separate ADC issue; no HX711
refactor is included. Real cabinet verification is still required after flashing.
