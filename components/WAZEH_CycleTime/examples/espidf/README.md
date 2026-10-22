# WAZEH_CycleTime examples

Each example is an independent ESP-IDF project and can be built from its own directory. The project CMake files locate the component in the parent component tree.

| Example | Purpose | Wiring |
|---|---|---|
| `01_pulse_counter` | Count rising edges from an external source | Signal to GPIO5; common ground |
| `02_dual_input_loopback` | Verify two independent inputs see the same slow waveform | GPIO4 output to GPIO5 and GPIO6 inputs |
| `03_switch_timing` | Observe switch press/release and run/stop/cycle timing | Momentary switch from GPIO5 to GND |

For every example, run `idf.py set-target esp32s3`, `idf.py build`, and `idf.py -p COMx flash monitor` from that example's directory. Change `COMx` to the correct port.

All pins are examples only. Confirm pin availability on the exact board before connecting anything. ESP32 GPIOs are not 5 V tolerant. Disconnect external circuitry from GPIO4/5/6 before running the loopback output example.
