# Example 01 — External pulse counter

Counts rising edges on GPIO5 and reports the cumulative count plus an estimated rate every second.

## Wiring

- Connect the pulse source to GPIO5.
- Connect source ground to ESP32 ground.
- Use a signal compatible with the ESP32's 3.3 V GPIO. Do not apply 5 V.

## Build and run

From this directory in an ESP-IDF terminal:

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

Replace `COMx` with your serial port. `GLITCH_FILTER_NS` is set to zero by default. Select a nonzero filter only after checking the target's PCNT filter constraints and the minimum valid pulse width. The interrupt fallback is not suitable for guaranteed high-frequency counting.
