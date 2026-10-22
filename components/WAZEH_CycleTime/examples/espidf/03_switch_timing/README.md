# Example 03 — Switch events and timing

Demonstrates a momentary active-low switch input on GPIO5 with an internal pull-up and 30 ms debounce. The application calls `loop()` regularly and reports press/release events, event duration, event count, and the cycle-time API result.

## Wiring

Connect a momentary normally-open switch between GPIO5 and GND. Do not drive GPIO5 high or low from another output while this example is running.

## Build and run

From this directory in an ESP-IDF terminal:

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

Replace `COMx` with your serial port. The exact meaning of timing values depends on the component's event state machine; use them as a functional demonstration and validate behavior against your application requirements.
