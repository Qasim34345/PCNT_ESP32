# Changelog

## Hardened example and component review update

- Initialized default-constructor debounce/repeat values.
- Improved early GPIO configuration rollback and logged PCNT-to-interrupt fallback.
- Reworked the demo to use checked APIs, cumulative count snapshots, a 5-second measurement window, input-edge diagnostics, explicit backend reporting, and automatic fallback when PCNT is demonstrably stalled.
- Expanded Unity tests to cover GPIO4-to-GPIO5 PCNT and GPIO-interrupt pulse counting and cumulative reset semantics.
- Updated build, test, counter semantics, and production qualification documentation.

This update has not been compiled or run on physical ESP32-S3 hardware in the current environment. Treat it as a candidate for validation, not a production certification.
