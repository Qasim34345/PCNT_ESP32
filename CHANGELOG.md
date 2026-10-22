# Changelog

## Responsive loopback measurement

- Changed live-status reports to a one-second cadence.
- Report live frequency and measured pulse cycle time from cumulative counter changes and elapsed time between edges; retain the last valid values between expected edges and clear them with the active pulse count after pulse inactivity.
- Removed GPIO4/GPIO5 level-mismatch disconnect detection to avoid false stale indications from asynchronous level reads.
- Keep the cumulative pulse total separate from live rate and connection status.
- Made the per-input PCNT watchdog interval track the configured signal period, with a 2.5-period verification window and 500 ms minimum.
- Reuse the shared GPIO ISR service instead of calling its installation API for every configured channel.
- Configure PCNT level actions to KEEP when no physical level/control GPIO is used, allowing edge-only counting.
- Let the application select its input pins/count and test simultaneous two-input hardware PCNT counting; inputs beyond the target's PCNT resources can use per-channel GPIO-interrupt fallback.

## Hardened example and component review update

- Initialized default-constructor debounce/repeat values.
- Improved early GPIO configuration rollback and logged PCNT-to-interrupt fallback.
- Reworked the demo to use checked APIs, cumulative count snapshots, input-edge diagnostics, explicit backend reporting, and automatic fallback when PCNT is demonstrably stalled.
- Expanded Unity tests to cover GPIO4-to-GPIO5 PCNT and GPIO-interrupt pulse counting and cumulative reset semantics.
- Updated build, test, counter semantics, and production qualification documentation.

This update has not been compiled or run on physical ESP32-S3 hardware in the current environment. Treat it as a candidate for validation, not a production certification.


## Counter lifecycle and recovery hardening

- Replaced the dynamically growing channel vector with a compile-time fixed-capacity channel table (`WAZEH_CYCLETIME_MAX_CHANNELS`, default 16) and nothrow channel allocation.
- Made PCNT-to-interrupt fallback independent of a successful PCNT read by preserving the last known valid cumulative total; logs explicitly warn that pulses since the last valid snapshot may be lost.
- Preserved cumulative totals across counter reconfiguration.
- Made owned-PCNT reset stop/snapshot/clear/restart and rejected resets on borrowed PCNT units to avoid clearing a shared counter.
- Improved GPIO configuration rollback and idempotent PCNT cleanup after already-stopped/disabled states.
- Changed the loopback demonstration to a constant 1 Hz target and improved the watchdog to detect a later counter stall, not only a counter stuck at its initial value.
- Added tests for reconfiguration continuity, falling/both-edge fallback counting, and hardware PCNT reset continuity.
- This remains a validation candidate until compiled and run with ESP-IDF on the target hardware.
