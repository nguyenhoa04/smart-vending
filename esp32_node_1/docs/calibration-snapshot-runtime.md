# Persistent calibration + WEIGHT_SNAPSHOT repair

Validated on 2026-10-06 against firmware base `ef4b89c` on
`feat/calibration-store`, within Backend branch `refactor/restructure-monorepo-v2`.

1. **Root cause.** `calibration_store` existed, but the active `shelf_service`
   did not depend on it or call its init/load/save functions. Startup used
   compiled scales 32 and 1, then unconditionally tared both occupied shelves.
   TARE/CALIBRATE only updated RAM. The snapshot change inherited this existing
   runtime rather than removing working calibration integration. The previous
   snapshot audit missed the disconnected calibration component.

2. **Files changed** (paths relative to `esp32_node_1`):

   ```text
   components/calibration_store/calibration_store.c
   components/shelf_service/CMakeLists.txt
   components/shelf_service/shelf_service.c
   components/command_service/command_service.c
   tests/run_weight_snapshot_tests.py
   tests/weight_snapshot_test.c
   tests/run_calibration_runtime_tests.py
   tests/calibration_runtime_test.c
   tests/host_stubs/esp_err.h
   tests/host_stubs/nvs.h
   tests/host_stubs/nvs_flash.h
   docs/calibration-snapshot-runtime.md
   ```

   No Pi, Backend, Flutter, HX711 driver, fusion, outbox, camera, or final
   reconciliation code changes. Existing local build directories were left
   alone. A separate temporary build directory contains the verified binary.

3. **CMake.** `shelf_service` adds `calibration_store` to `PRIV_REQUIRES` alongside
   `hx711` and `pi_uart`. The store's existing `nvs_flash` dependency is reused.
   No new external dependencies or sdkconfig changes.

4. **Initialization.** The existing `node_app_start()` order initializes UART
   first, then calls `shelf_service_init()`. That function is the single owner
   of `calibration_store_init()`, whose existing ready guard is idempotent.
   Both records and their validity flags are loaded/applied before HX711
   initialization, warmup, and initial raw-sample discard. Sensor/command tasks
   start afterward. Boot performs no calibration writes.

5. **Startup tare, before/after.** Previously startup sampled and tared each
   shelf regardless of contents. Now startup never tares, including when a
   record is missing or invalid. Zero offset/scale placeholders are explicitly
   invalid, not fallback calibrations. Each shelf needs both valid flags and
   a finite scale with absolute value at least the store's existing 0.5 limit.
   Negative scales remain valid for the corresponding load-cell wiring.
   Uncalibrated shelves keep raw reads and explicit maintenance commands but
   publish no `TOTAL`. Logs say `calibration required`; `STATUS` returns
   `weight=nan` for an invalid/uninitialized/stale/busy primary shelf. Pi's
   existing finite-value validation rejects that value.

6. **TARE persistence.** `TARE` and `TARE:SHELF_n` sample the selected shelf,
   update only its offset/offset-valid flag, preserve scale/scale-valid, and
   use the existing blob save plus `nvs_commit()`. Only successful persistence
   updates HX711 RAM, resets that shelf's filter, and returns an OK response.
   Tare is an explicit operator action on an empty shelf, never a boot action.
   With no previous calibration it establishes offset only; weights remain
   unavailable until explicit scale calibration succeeds.

7. **CALIBRATE persistence.** Both command forms require a valid offset and a
   finite positive known weight. They preserve offset/offset-valid and persist
   the measured scale/scale-valid before applying it and resetting the filter.
   Invalid scale, failed samples, or NVS errors produce ERROR and retain current
   RAM/filter state. Nonfinite input is rejected by both parser and service.
   A storage error may leave its on-flash outcome uncertain; no rollback of
   NVS is claimed. The next boot validates/reloads the stored record. Never
   return a successful ACK for a failed save/commit.

8. **Snapshot regression.** The UART command and `SHELF_n: TOTAL=...` protocol
   are unchanged. `shelf_service_send_all_snapshots()` still takes a bounded
   50ms mutex, copies recent filtered samples, releases the mutex, then sends
   UART output. The 1000ms age limit, busy/stale logs, and tick-rollover handling
   remain. Calibration validity is an additional prerequisite. Snapshot does
   not read HX711, tare, calibrate, change offsets/scales/filter/publication
   state, write NVS, or unlock. Five repeated requests in each calibrated host
   scenario return correct readings without a physical shelf interaction.

9. **Tests.** All existing firmware host tests pass: **7 snapshot groups**.
   New tests compile the production shelf/command/node startup code and real
   calibration store against mocked HX711, FreeRTOS, UART, and NVS primitives:
   **9 scenarios, 14 passing groups**, including 8 invalid-record variants.
   They cover all 24 requested checks: both legacy records/flags applied before
   HX initialization, no startup tare/write, restart reload, missing/partial
   calibration, warnings/status, both maintenance command forms, preserved
   fields/other shelf, write/commit/sample failures, snapshot state byte-for-byte
   unchanged, occupied-shelf nonzero output, and repeated two-shelf snapshots.
   Existing snapshot tests keep all original assertions and add calibration
   fixtures plus a zero-NVS-call assertion.

   Pi baseline-weight-prime tests: **18 passed**. Full existing Python suite:
   **700 passed in 52.84s**. No Flutter changes. Hardware calibration accuracy,
   physical reboot, and a live QR/unlock session still require cabinet testing.

   From the main repository root:

   ```sh
   python3 firmware/esp32_node_1/tests/run_weight_snapshot_tests.py
   python3 firmware/esp32_node_1/tests/run_calibration_runtime_tests.py
   .venv313/bin/python -m pytest -q
   ```

10. **ESP32 build.** Full clean build passed with the installed ESP-IDF checkout
    `v6.1-dev-6126-ge9da155a726`, Python 3.13 environment, and Xtensa GCC 16.1.0.
    Binary: `/private/tmp/calibration-snapshot-esp32-build/esp32_node.bin`,
    **206480 bytes (0x32690)**, with **80%** app partition free. Build log:
    `/private/tmp/calibration-snapshot-build.txt`. Existing ESP-IDF Kconfig
    notes remain; no application compile failures. The first sandbox attempt
    was blocked by the component manager's system-process query; the approved
    build outside the sandbox succeeded. No device flash was performed.

11. **Expected logs** (placeholders represent the actual stored values, not
    constants introduced by this fix):

    ```text
    MAIN_APP: System is starting...
    PI_UART: UART initialized successfully
    CAL_STORE: Calibration NVS ready
    MAIN_APP: SHELF_1 calibration loaded | offset=<stored> | scale=<stored> | valid=yes
    MAIN_APP: SHELF_2 calibration loaded | offset=<stored> | scale=<stored> | valid=yes
    HX711: <existing shelf initialization messages>
    MAIN_APP: Warming up 2 independent HX711 converter(s) for 2 seconds...
    MAIN_APP: Startup auto-tare disabled, using persisted calibration only
    MAIN_APP: SHELF_1 -> TOTAL: <calibrated grams> | RAW: ... | OFFSET: ...
    MAIN_APP: SHELF_2 -> TOTAL: <calibrated grams> | RAW: ... | OFFSET: ...
    ```

    `Auto-taring shelf ... on startup` must not appear. After QR authorization,
    repeated WEIGHT_SNAPSHOT supplies both fresh, calibrated totals using the
    unchanged UART format. Pi can then reach BASELINE_WEIGHT_READY for both
    active shelves, run baseline inference/validation, and unlock according
    to its existing policy. No physical touch is needed to request readings.

12. **Exact flash command on this Mac.** `/dev/cu.usbserial-0001` was present
    during validation. Run from the firmware project using this explicit
    build directory to select the verified new binary:

    ```sh
    cd /Users/thanhhoa/Documents/Backend/firmware/esp32_node_1
    source /Users/thanhhoa/.espressif/python_env/idf6.2_py3.13_env/bin/activate
    source /Users/thanhhoa/esp/esp-idf/export.sh
    idf.py -B /private/tmp/calibration-snapshot-esp32-build -D SDKCONFIG=/private/tmp/calibration-snapshot-sdkconfig -p /dev/cu.usbserial-0001 flash monitor
    ```

    To rebuild if temporary files were cleared, use the same environment:

    ```sh
    cp sdkconfig /private/tmp/calibration-snapshot-sdkconfig
    idf.py -B /private/tmp/calibration-snapshot-esp32-build -D SDKCONFIG=/private/tmp/calibration-snapshot-sdkconfig build
    ```

13. **Migration.** None. Namespace `shelf_cal`, keys `shelf_1`/`shelf_2`, magic
    `0x43414C31`, version 1, 20-byte blob layout, flags, and existing validation
    are preserved. Legacy-format blobs are seeded directly in host tests to
    verify compatibility; no copied example calibration values in production.

14. **Existing NVS.** Startup now reads valid existing records without writes.
    The former automatic erase on NO_FREE_PAGES/NEW_VERSION_FOUND is removed;
    such failures preserve NVS and require explicit operator maintenance.
    The generated flash arguments write bootloader at 0x1000, partition table
    at 0x8000, and application at 0x10000. The unchanged single-app partition
    table places NVS at 0x9000, size 0x6000; none of those images overlap it.
    No erase-flash or NVS-image write is required. Actual cabinet NVS records
    have not been read in this task, so their prior existence/integrity cannot
    be guaranteed. Whatever valid records remain are loaded unchanged.
