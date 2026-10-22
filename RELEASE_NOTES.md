# WAZEH_CycleTime 1.1.0 — Production Candidate

## Summary

This candidate hardens the ESP-IDF component and the ESP32-S3 loopback diagnostic example. It is not a production certification; hardware qualification remains mandatory.

## Component changes

- Uses a fixed-capacity channel table (default 16) instead of a dynamically growing vector. Configure `WAZEH_CYCLETIME_MAX_CHANNELS` at compile time to change the limit.
- Configures PCNT level actions to KEEP when no physical level/control GPIO is selected, allowing the configured rising, falling, or both-edge actions to update the counter.
- Supports application-selected input counts through repeated `addChannel()` calls. PCNT resources remain target-limited; per-channel GPIO-interrupt fallback is available when hardware PCNT allocation fails.
- Uses nothrow allocation for channel objects and reports `ESP_ERR_NO_MEM` from `addChannel()` when allocation fails or the channel table is full.
- Preserves the last known cumulative total when switching from PCNT to the GPIO-interrupt backend after a PCNT read failure. Logs warn that pulses after the last valid snapshot may be lost.
- Preserves cumulative totals during counter reconfiguration.
- Synchronizes software-counter snapshots and resets with the ISR critical section.
- Stops, snapshots, clears, and restarts an internally owned PCNT unit during reset. A borrowed PCNT unit cannot be reset by this component because that could affect other users; `resetPulseCountChecked()` returns `ESP_ERR_NOT_SUPPORTED` for borrowed units.
- Improves GPIO setup rollback and cleanup behavior for PCNT units that are already stopped or disabled.
- Removes unused global debounce state; `setDebounceDelay()` updates currently configured channels, while typed channel configurations retain their own `debounce_ms` values.

## Example and tests

- The ESP32-S3 example generates a constant 1 Hz, 50% duty-cycle signal on GPIO4 and counts rising edges independently on each input listed in the application's `monitors[]` initializer (GPIO5 and GPIO6 by default).
- Reports per-input live frequency and cycle time once per second, plus active and cumulative counts.
- The watchdog checks for a counter that fails to advance after observed input activity, including a counter that stalls after previously working.
- Unity tests cover API validation, repeated cleanup, single- and dual-input PCNT loopback counting, GPIO-interrupt loopback counting, cumulative reset behavior, counter reconfiguration continuity, and falling/both-edge software counting.

## Qualification limitations

- The source passed syntax-only C++ checks using temporary API stubs for PCNT-enabled and PCNT-disabled branches. These checks do not validate ESP-IDF API compatibility, linking, target-specific PCNT behavior, or timing.
- No ESP-IDF 6.1 build, Unity test execution, or physical ESP32-S3 measurement was possible in this environment.
- Do not treat automatic fallback as proof that PCNT is healthy. During qualification, preserve logs and investigate every unexpected backend switch.
- A software interrupt counter can lose/coalesce edges under high interrupt load and must not be used for safety-critical counting without target-specific validation.
