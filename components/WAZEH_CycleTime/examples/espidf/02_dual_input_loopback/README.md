# Example 02 — Dual-input loopback

Generates a nominal 1 Hz, 50% duty-cycle square wave on GPIO4 and counts rising edges independently on GPIO5 and GPIO6.

## Wiring

```text
GPIO4 output ──┬── GPIO5 input
               └── GPIO6 input
```

Use only these pins if they are available on your specific board. Disconnect external circuitry from them before running the example. Never apply more than 3.3 V to an ESP32 GPIO.

## Build and run

From this directory in an ESP-IDF terminal:

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

The timer-task GPIO waveform is intentionally slow and is only for functional loopback checks. For accurate or high-frequency characterization, use a hardware waveform peripheral or external signal generator and a suitable measurement instrument.
