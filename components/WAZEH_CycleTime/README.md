# WAZEH_CycleTime ESP-IDF Component

`WAZEH_CycleTime` is an ESP-IDF C++ component for GPIO input events, debounce/repeat handling, machine run/stop and cycle-time measurements, and pulse counting using ESP32 PCNT hardware when available, with an optional GPIO-interrupt counter fallback.

> **Status:** This package is a development baseline, not a certified production release. Build it with your intended ESP-IDF version and validate it on the exact target board and electrical interface before production use. This component retains its proprietary/confidential license notice; use and redistribution must be authorized by its owner.

## Features

- Typed channel configuration through `WAZEH_CycleTime_ChannelConfig`.
- Multiple independent GPIO input channels, limited by `WAZEH_CYCLETIME_MAX_CHANNELS` (default 16).
- Pushbutton/switch event processing, configurable active level and pull configuration, and debounce timing.
- Run time, stop time, cycle time, and event counters.
- Checked pulse-counter initialization, configurable rising/falling/both-edge selection, and optional GPIO-interrupt fallback.
- Checked 64-bit cumulative pulse-count reads using `getPulseCount()`.
- Counter backend/error inspection through `getChannel()`.

## Directory layout

```text
WAZEH_CycleTime/
├── CMakeLists.txt
├── README.md
├── include/                 # Public API and configuration
├── src/                     # Component implementation
├── examples/                # Small standalone ESP-IDF demo projects
└── test_apps/               # Unity/API/hardware regression tests
```

## Requirements

- ESP-IDF project using C++17.
- An ESP32 target supported by the selected ESP-IDF release and its GPIO/PCNT drivers. PCNT availability and resources vary by target.
- Correctly wired 3.3 V GPIO signals; never apply 5 V to an ESP32 GPIO.

The component CMake file declares dependencies on `esp_driver_gpio`, `esp_driver_pcnt`, `freertos`, and `esp_timer`.

## Quick start

1. Copy `WAZEH_CycleTime` into your project's `components/` directory, or point `EXTRA_COMPONENT_DIRS` at the directory containing this component.
2. Include `WAZEH_CycleTime.h` in your C++ application.
3. Create a `WAZEH_CycleTime` object and add one typed channel per input.
4. Initialize pulse counting if required, check every returned `esp_err_t`, and call `loop()` regularly from task context for event/machine timing.
5. Prefer `getPulseCount(pin, &count)` for checked 64-bit cumulative counts. Avoid the legacy `long` count APIs for long-running/high-count applications.

Example channel setup:

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

if (err == ESP_OK) {
    err = cycleTime.initPulseCounterByPinChecked(
        GPIO_NUM_5, WAZEH_CycleTime_Edge::Rising, 0, true);
}

uint64_t totalPulses = 0;
if (err == ESP_OK) {
    err = cycleTime.getPulseCount(GPIO_NUM_5, &totalPulses);
}
```

## Examples

Each directory under `examples/` is a separate ESP-IDF project. Open an ESP-IDF terminal in the selected example directory and run:

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

Replace `COMx` with your board's serial port. Read the example's own README before wiring or flashing.

- `examples/01_pulse_counter`: count an externally generated pulse signal on one GPIO.
- `examples/02_dual_input_loopback`: drive one slow test waveform and count it independently on two input pins.
- `examples/03_switch_timing`: demonstrate switch events and run/stop/cycle timing.

The loopback example's software-timer waveform is for basic functional testing only, not precision or high-frequency qualification.

## Tests versus examples

Keep `test_apps/` and `examples/` separate:

- **Examples** are small, readable demonstrations for users to copy, adapt, and run manually.
- **Test apps** are regression/validation projects, usually with assertions and controlled pass/fail outcomes, used to detect defects after changes.

Some code may overlap, but their goals are different. A test passing does not establish high-frequency accuracy or production qualification.

## Important limitations

- The component is not thread-safe. Configure, read, reset, process, and deinitialize it from one application task unless synchronization is added by the application/component design.
- Do not call component APIs from an ISR. Keep callbacks short and nonblocking.
- GPIO-interrupt fallback may miss pulses under interrupt latency or system load; it is not a replacement for PCNT at high frequencies.
- PCNT limits, filter behavior, and available hardware resources depend on the target and ESP-IDF version. Multiple configured channels do not guarantee that every channel obtains a hardware PCNT unit.
- Pulse-rate calculations are estimates based on samples; they are not calibrated measurements. Precise individual-edge timing requires a suitable hardware capture approach.
- Reset/reconfiguration may have a brief transition interval where pulses can be missed. Inspect return codes and backend/error state.
- Verify the exact board schematic, pin availability, input voltage, grounding, signal integrity, and glitch-filter settings before testing.

## Validation status

The examples are provided as source projects. They have not been compiled against your local ESP-IDF installation or verified on your physical board as part of this package preparation. Build and run each example on the intended target before relying on it.
