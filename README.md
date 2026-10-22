# WAZEH_CycleTime ESP-IDF component and ESP32-S3 validation example

This project contains the `WAZEH_CycleTime` component, a configurable ESP32-S3 GPIO loopback demonstration, and a Unity hardware/API validation application. It provides GPIO event processing, debounce/repeat events, run/stop and cycle timing, pulse counting, rate estimation, and rolling 60-second count statistics.

**Qualification status:** Version 1.1.0 is a hardened production-candidate baseline, not a production certification. It must be built with the intended ESP-IDF release and tested on the actual board and electrical interface before production use. The source retains its proprietary/confidential license notice; use and redistribution must be authorized by the owner.

## 1. Dual-input loopback example

The main app generates a 50% duty-cycle square wave on GPIO4 at a default target of 1 Hz and counts rising edges independently on each configured input. The selected input count and pins are set by the `monitors[]` initializer in `main/app_main.cpp`; its default entries are GPIO5 and GPIO6. GPIO4 is wired in parallel to all listed input pins. Set `OUTPUT_FREQUENCY_HZ` to the desired positive frequency; the timer period is represented in microseconds, so actual output is limited by timer resolution and task-dispatch jitter.

```text
ESP32-S3 GPIO4 (output)  --------+-----------> GPIO5 (input / counter 1)
                                 +-----------> GPIO6 (input / counter 2)
```

For the default configuration, connect GPIO4 to both GPIO5 and GPIO6. For other input sets, connect GPIO4 to every GPIO listed in `monitors[]`. The inputs are high impedance; do not configure or wire them as outputs. Never apply more than 3.3 V to an ESP32 GPIO. Check the exact board schematic and pin availability before selecting inputs. External equipment also requires a common ground.

The example:

- Uses the checked API and explicit typed channel configuration.
- Adds one component channel per application-selected input pin and initializes each counter independently. Hardware PCNT is preferred per channel; GPIO-interrupt fallback and watchdog/recovery are also managed independently. If GPIO activity is detected but PCNT does not advance, the app verifies the backend for 2.5 target periods (2.5 seconds at 1 Hz) before switching that input to GPIO-interrupt counting.
- Reports pin-labelled status once per second: one `OUTPUT GPIO4=...` line followed by one `INPUT GPIO...=...` line per configured input. Each input line includes its level, counter backend/read status, live frequency, cycle time in milliseconds, active and cumulative counts, and diagnostic edge count.
- Keeps each 64-bit total count cumulative. Each live frequency and cycle time are estimated from that channel's rising-edge count and elapsed time; they remain at the last valid measurement between expected edges and clear after 1.5 target periods without a new edge.
- Tracks rising edges separately for each channel's PCNT watchdog. This polling diagnostic is suitable for this low-rate loopback demo, not a general-purpose high-frequency reference.
- Installs the shared GPIO ISR service once per component process rather than retrying the installation for each input channel.
- Uses a fixed-capacity channel table (`WAZEH_CYCLETIME_MAX_CHANNELS`, default 16) instead of a dynamically growing vector. This avoids vector reallocation in the component and returns `ESP_ERR_NO_MEM` if the table is full or a channel allocation fails.
- Inputs driven by the same source should measure approximately the same rate and cycle time, and their cumulative counts should remain close. Slight initial count differences may occur because the channels are configured sequentially. A fault or fallback on one input does not switch another input's backend.
- The component channel table defaults to 16 channels (`WAZEH_CYCLETIME_MAX_CHANNELS`). The app may select any number up to that configured capacity. Hardware PCNT resources are target-limited and may be fewer than the selected inputs; when a PCNT allocation fails, the checked initialization API can activate GPIO-interrupt fallback for that channel.

The fallback is a recovery/diagnostic feature, not a substitute for investigating a PCNT mismatch. If fallback activates, preserve the log and test the PCNT loopback Unity tests below. A timer task produces a diagnostic waveform, not precision timing; use a hardware waveform peripheral if precise output timing is required.

Example log format:

```text
Sample 246 | OUTPUT GPIO4=LOW(0) | target=1.000 Hz | toggles=2
Sample 246 | INPUT GPIO5=LOW(0) | backend=GPIO-ISR | counter_read=OK | live rate=1.000 Hz | cycle time=1000.004 ms | live pulses=239 | total pulses=239 | pulse=ACTIVE | diagnostic_rising=246
Sample 246 | INPUT GPIO6=LOW(0) | backend=PCNT | counter_read=OK | live rate=1.000 Hz | cycle time=1000.003 ms | live pulses=239 | total pulses=239 | pulse=ACTIVE | diagnostic_rising=246
```

Each line explicitly names its GPIO, so `OUTPUT` is GPIO4 and each `INPUT` line corresponds to one entry in `monitors[]`. If GPIO4 toggles while an input's `diagnostic_rising` and total count do not increase, inspect that input's jumper/wiring, board pin availability, and counter backend.

## 2. Build and flash the example

Use an ESP-IDF 6.1 terminal, then from this project's root:

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

Replace `COMx` with the correct serial port. The included defaults select ESP32-S3 and 16 MB flash. Confirm the actual flash size of your board; do not flash a configuration that assumes a larger flash chip than installed.

Each input's live rate is based on cumulative counter changes and elapsed time between rising edges; it is not an average over a long window. Three decimal places are display precision, not a guarantee of ±0.001 Hz accuracy. At very low frequencies, the estimate updates on pulse arrivals, while the 1.5-period inactivity timeout marks the rate zero when pulses stop. Timer task dispatch and the GPIO-interrupt fallback can introduce measurement jitter or missed edges, so this example is not a calibrated precision instrument.

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
- Simultaneous hardware PCNT rising-edge counting on GPIO5 and GPIO6 from one GPIO4 loopback output.
- GPIO-interrupt fallback rising-edge counting.
- Cumulative count preservation across software and hardware counter resets.
- Cumulative count preservation across counter reconfiguration.
- Software fallback falling-edge and both-edge selection.

The test app drives GPIO4 and reads GPIO5; the dual-input PCNT test also reads GPIO6. Connect GPIO4 to GPIO5 and GPIO6 on the board for the corresponding tests. These tests actively toggle the output and should be run with the board disconnected from any external circuit on those pins.

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
- `resetPulseCountChecked()` starts a new raw interval while preserving the cumulative total. For an internally owned PCNT unit, it stops the unit before snapshot/clear/restart; a transition during that brief stopped interval may not be counted. For a borrowed/external PCNT unit, reset returns `ESP_ERR_NOT_SUPPORTED` because clearing the unit could affect other users. For precise interval measurement, prefer cumulative snapshots and subtract successive readings, as the example does.
- Reinitializing a counter attempts to preserve the last cumulative total. If the old backend cannot be read, only the last known good total can be preserved; pulses since that snapshot may be lost, and the component logs this condition.
- The component supports up to `WAZEH_CYCLETIME_MAX_CHANNELS` configured channels (default 16). Change the macro consistently at compile time if the application requires a different fixed capacity.
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
