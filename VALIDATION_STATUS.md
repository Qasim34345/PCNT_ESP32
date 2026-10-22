# Validation status — WAZEH_CycleTime update

## Changes made

- Initialized default `WAZEH_CycleTime` debounce and repeat values.
- Improved GPIO initialization rollback on setup failures and made PCNT stop/disable cleanup more tolerant of already-stopped/disabled states.
- Added saturation for event counts and run/stop accumulators.
- Initialized active-event and cycle timing when a channel starts in its active state.
- Cleared borrowed external PCNT handles after the component-owned channel is removed.
- Avoided recording the same pulse-statistics sample twice in `getPulseCountLastMinute()`.
- Reworked the ESP32-S3 example to use checked APIs, app-selected input pins, per-channel cumulative counts, one-second status reporting, independent low-rate edge diagnostics, explicit backend reporting, a default 1 Hz output, and per-channel PCNT watchdog/fallback.
- Replaced the dynamically growing channel vector with a fixed-capacity table (default 16 channels) and nothrow channel allocation.
- Made fallback after a PCNT read error independent of a successful second PCNT read, preserving the last known valid cumulative count and logging possible lost pulses since that snapshot.
- Preserved cumulative counts across counter reconfiguration, improved owned-PCNT reset semantics, and rejected clearing borrowed PCNT units.
- Added Unity loopback tests for hardware PCNT, GPIO-interrupt fallback, repeated cleanup, reset continuity, reconfiguration continuity, and falling/both-edge fallback counting.
- Removed generated test build outputs and host-specific VS Code settings from the portable source archive; project source and relevant configuration files remain included.
- Corrected PCNT level actions to KEEP when the PCNT level/control GPIO is virtual, so edge counting is not held by the absent control input.
- Changed the demo input list to be selected by the application and added a dual-input hardware PCNT loopback test. Hardware PCNT availability is target-limited; additional application channels may use the per-channel GPIO-interrupt fallback.
- Changed the PCNT watchdog to a 2.5-target-period window (500 ms minimum) and reused the shared GPIO ISR service across channels.

## Checks performed in this environment

- Inspected the uploaded project source and configuration.
- Applied source-level consistency checks and reviewed the edited code paths.
- C++ syntax-only checks passed for the component, example, and Unity test source in both PCNT-enabled and PCNT-disabled preprocessor branches using temporary API stubs. This does not substitute for compiling against the actual ESP-IDF headers or linking the firmware.
- Confirmed the portable package excludes generated build artifacts and includes the component, example, test sources, configuration, and documentation.
- Reviewed the counter ownership contract: the component will not clear a borrowed PCNT unit, because it may be shared with other channels.

## Checks not performed

- The latest PCNT setup, multi-input application, and dual-input test changes have not been built or executed.
- No ESP-IDF compiler or `idf.py` installation is available in this execution environment, so the updated project was **not compiled** here.
- The updated Unity tests were **not executed**.
- No ESP32-S3 hardware was available, so GPIO4-to-GPIO5 loopback and PCNT behavior were **not measured**.

## Release gate

Do not mark this component production-qualified until a clean ESP-IDF 6.1 build and the included hardware tests pass on the actual target, followed by frequency/edge-accuracy, long-duration, noise, resource-cleanup, and application-load tests appropriate to product requirements. The demo's automatic fallback is diagnostic resilience; it must not be used to hide a PCNT failure during qualification testing.
