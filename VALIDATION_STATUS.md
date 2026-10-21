# Validation status — WAZEH_CycleTime update

## Changes made

- Initialized default `WAZEH_CycleTime` debounce and repeat values.
- Improved earlier GPIO-interrupt rollback on setup failures.
- Added saturation for event counts and run/stop accumulators.
- Initialized active-event and cycle timing when a channel starts in its active state.
- Cleared borrowed external PCNT handles after the component-owned channel is removed.
- Avoided recording the same pulse-statistics sample twice in `getPulseCountLastMinute()`.
- Reworked the ESP32-S3 example to use checked APIs, cumulative count snapshots, a 5-second measurement interval, an independent low-rate input-edge diagnostic, explicit backend reporting, and automatic fallback when PCNT demonstrably fails to advance.
- Added Unity loopback tests for hardware PCNT, GPIO-interrupt fallback, repeated PCNT cleanup, and cumulative count preservation across resets.
- Removed generated test build outputs and host-specific VS Code settings from the portable source archive; project source and relevant configuration files remain included.

## Checks performed in this environment

- Inspected the uploaded project source and configuration.
- Applied source-level consistency checks and reviewed the edited code paths.
- Confirmed the portable package excludes generated build artifacts and includes the component, example, test sources, configuration, and documentation.

## Checks not performed

- No ESP-IDF compiler or `idf.py` installation is available in this execution environment, so the updated project was **not compiled** here.
- The updated Unity tests were **not executed**.
- No ESP32-S3 hardware was available, so GPIO4-to-GPIO5 loopback and PCNT behavior were **not measured**.

## Release gate

Do not mark this component production-qualified until a clean ESP-IDF 6.1 build and the included hardware tests pass on the actual target, followed by frequency/edge-accuracy, long-duration, noise, resource-cleanup, and application-load tests appropriate to product requirements. The demo's automatic fallback is diagnostic resilience; it must not be used to hide a PCNT failure during qualification testing.
