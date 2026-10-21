# WAZEH_CycleTime ESP-IDF component and ESP32-S3 validation example

This project contains the `WAZEH_CycleTime` component, a configurable ESP32-S3 GPIO loopback demonstration, and a Unity hardware/API validation application. It provides GPIO event processing, debounce/repeat events, run/stop and cycle timing, pulse counting, rate estimation, and rolling 60-second count statistics.

**Qualification status:** This codebase is a hardened development baseline, not a production certification. It must be built with the intended ESP-IDF release and tested on the actual board and electrical interface before production use. The source retains its proprietary/confidential license notice; use and redistribution must be authorized by the owner.

## 1. Loopback example

The main app generates a 50% duty-cycle square wave on GPIO4 and counts rising edges on GPIO5. Set `OUTPUT_FREQUENCY_HZ` in `main/app_main.cpp` to the desired positive frequency; the timer period is represented in microseconds, so the actual output is limited by timer resolution and task-dispatch jitter.

```text
ESP32-S3 GPIO4 (output)  --------------------> GPIO5 (input)
ESP32-S3 GND             --------------------  GND (only if external equipment is attached)
```

Use a jumper wire for GPIO4-to-GPIO5 loopback. Never connect two actively driven outputs together and never apply more than 3.3 V to an ESP32 GPIO. Confirm the exact board schematic before using GPIO4/GPIO5.

The example:

- Uses the checked API and explicit typed channel configuration.
- Prefers hardware PCNT and logs the active backend and initialization error.
- Uses cumulative pulse counts and actual elapsed time; it does not clear PCNT at each sample boundary.
- Reports both the rate measured within each 5-second sample and the running average since counting began. The running average reduces the impact of pulse-boundary quantization over time.
- Tracks input rising edges independently in the application task for a low-rate diagnostic cross-check.
- If GPIO5 demonstrably sees edges while PCNT remains stuck at zero, logs a clear error and switches to the component's GPIO-interrupt fallback.
- Reports interval count, cumulative count, interval and running-average frequency to three decimal places, backend, input/output levels, and signal-generator toggle activity.

The fallback is a recovery/diagnostic feature, not a substitute for investigating a PCNT mismatch. If fallback activates, preserve the log and test the PCNT loopback Unity test below.

## 2. Build and flash the example

Use an ESP-IDF 6.1 terminal, then from this project's root:

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

Replace `COMx` with the correct serial port. The included defaults select ESP32-S3 and 16 MB flash. Confirm the actual flash size of your board; do not flash a configuration that assumes a larger flash chip than installed.

The interval rate can fluctuate because each 5-second window contains a whole number of pulses; the count may differ by one pulse at a window boundary. The running average should converge toward the actual generated rate as the measurement duration grows. Three decimal places in the log are display precision, not a guarantee of ±0.001 Hz accuracy: with count-over-time measurement, resolving roughly 0.001 Hz requires on the order of 1,000 seconds of observation. Timer task dispatch also introduces jitter, so this example is not a precision waveform generator.

## 3. Run the validation test app

The Unity test app is in `components/WAZEH_CycleTime/test_apps/api_validation`.

```powershell
cd components/WAZEH_CycleTime/test_apps/api_validation
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

The tests cover:

- Invalid configuration and missing channel behavior.
- Default construction followed by typed channel addition.
- Repeated PCNT initialization and cleanup.
- Hardware PCNT rising-edge counting with GPIO4-to-GPIO5 loopback (ESP32-S3 target required).
- GPIO-interrupt fallback rising-edge counting.
- Cumulative count preservation across a raw counter reset.

The test app drives GPIO4 and reads GPIO5. Connect GPIO4 to GPIO5 on the board. These tests actively toggle the output and should be run with the board disconnected from any external circuit on those pins.

## 4. API example

```cpp
WAZEH_CycleTime cycleTime;
WAZEH_CycleTime_ChannelConfig config{};
config.pin = GPIO_NUM_5;
config.input_mode = CYCLETIME_INPUT_MODE_SWITCH;
config.active_level = WAZEH_CycleTime_ActiveLevel::High;
config.pull = WAZEH_CycleTime_Pull::Down;
config.debounce_ms = 20;

esp_err_t err = cycleTime.addChannel(config);
if (err != ESP_OK) {
    // Handle configuration failure.
}

err = cycleTime.initPulseCounterByPinChecked(
    GPIO_NUM_5, WAZEH_CycleTime_Edge::Rising, 10000, true);
// If err != ESP_OK, inspect the backend and last error: fallback may be active.
WAZEH_Channel *channel = cycleTime.getChannel(GPIO_NUM_5);
if (channel != nullptr) {
    const auto backend = channel->getCounterBackend();
    const esp_err_t counterError = channel->getLastCounterError();
}

uint64_t totalPulses = 0;
err = cycleTime.getPulseCount(GPIO_NUM_5, &totalPulses);
```

## 5. Counter semantics and limitations

- `getPulseCount()` returns the cumulative 64-bit count and an explicit error status.
- `getRawPulseCount()` is a legacy `long` API that returns `-1` on read failure; prefer the checked 64-bit API.
- `resetPulseCountChecked()` starts a new raw interval while preserving the cumulative total. As with most read-then-clear counter interfaces, a pulse exactly at the reset boundary may race with the operation. For precise interval measurement, prefer cumulative snapshots and subtract successive readings, as the example does.
- `calculatePulseRate()` computes count delta divided by elapsed time between calls. It needs at least two successful samples and returns the last calculated rate; use a controlled sampling interval for low-frequency signals.
- `getPulseCountLastMinute()` sums observed count deltas in 60 one-second buckets. Query regularly; if the component is not sampled, it cannot reconstruct the exact second in which each historical pulse occurred.
- PCNT is preferred for high-rate counting. The GPIO interrupt fallback may miss/coalesce transitions under interrupt latency or system load and is not appropriate for safety-critical counting without target-specific qualification.
- The component is not thread-safe. Configure, read, reset, process, and deinitialize from one application task. GPIO ISR code records edges only; call `loop()` regularly from task context for debounced events and machine timing.
- `getChannel()` returns a borrowed pointer valid only until successful `deinit()` or object destruction.
- The ISR service is shared globally and is intentionally not uninstalled by the component.
- Do not call component APIs from an ISR. Keep callbacks short and nonblocking.

## 6. Production qualification checklist

Before a production release, record and verify all of the following on the exact target board and ESP-IDF version:

1. Clean build of both the example and Unity test app with warnings treated as errors where practical.
2. PCNT loopback test passes repeatedly; backend logs show PCNT and the expected count.
3. GPIO-interrupt fallback test passes repeatedly and fallback behavior is explicitly accepted for the target pulse rate.
4. Frequency accuracy measured against a calibrated signal source at minimum, nominal, and maximum expected frequency.
5. Input voltage, edge rise/fall times, noise, cable length, grounding, and glitch-filter threshold verified electrically.
6. Repeated initialization/deinitialization and error-injection tests show no GPIO, ISR, or PCNT resource leaks.
7. Long-duration test covers count overflow, resets, uptime, and rolling-window semantics.
8. Application load, Wi-Fi/Bluetooth/other interrupts, watchdog behavior, and concurrent task activity tested.
9. Requirements, failure modes, acceptable error margins, and degraded-backend behavior documented.

A successful build or a few loopback tests alone do not establish production readiness.
