#ifndef WAZEH_CHANNEL_H
#define WAZEH_CHANNEL_H

#include "WAZEH_CycleTime_Config.h"

class WAZEH_Channel {
public:
    WAZEH_Channel(uint8_t pin, uint8_t mode);
    explicit WAZEH_Channel(const WAZEH_CycleTime_ChannelConfig &config);
    ~WAZEH_Channel();

    WAZEH_Channel(const WAZEH_Channel &) = delete;
    WAZEH_Channel &operator=(const WAZEH_Channel &) = delete;
    WAZEH_Channel(WAZEH_Channel &&) = delete;
    WAZEH_Channel &operator=(WAZEH_Channel &&) = delete;

    void configure();
    esp_err_t configureChecked();
    esp_err_t deinit();
    WAZEH_CycleTime_Event loop(unsigned long debounceDelay, unsigned long repeatDelay,
                               WAZEH_CycleTime_Event_Callback callback);
    WAZEH_CycleTime_Event loop(unsigned long debounceDelay, unsigned long repeatDelay,
                               const WAZEH_CycleTime_TypedEventCallback &callback);
    WAZEH_CycleTime_Event loop(unsigned long debounceDelay, unsigned long repeatDelay,
                               const WAZEH_CycleTime_Event_Callback &legacyCallback,
                               const WAZEH_CycleTime_TypedEventCallback &typedCallback);

    bool pressed() const;
    uint8_t getPin() const { return _pin; }
    unsigned long getEventLength() const;
    unsigned long getEventCount() const;
    uint64_t getEventCount64() const { return _eventCount; }
    uint64_t getEventLength64() const { return _eventLengthMs; }
    void setDebounceDelay(uint32_t delayMs) { _debounceDelayMs = delayMs; }
    uint32_t getDebounceDelay() const { return _debounceDelayMs; }

    unsigned long calculateRunTime();
    unsigned long calculateStopTime();
    unsigned long calculateCycleTime();
    WAZEH_CycleTime_RunStopResult calculateRunStopTime();

    long getRawPulseCount();
    esp_err_t getPulseCount(uint64_t *count);
    esp_err_t getLastCounterError() const { return _lastCounterError; }
    WAZEH_CycleTime_CounterBackend getCounterBackend() const;
    float calculatePulseRate();
    long calculatePulseCount();
    long getPulseCountLastMinute();

    void resetPulseCount();
    esp_err_t resetPulseCountChecked();
    bool isPcntInitialized() const { return _pcntInitialized; }
    bool isCounterAvailable() const { return _pcntInitialized || _usingSoftwareCounter; }
    void initPulseCounter(bool countBothEdges = false, uint16_t filter_us = 0);
    esp_err_t initPulseCounterChecked(WAZEH_CycleTime_Edge edge, uint32_t filter_ns,
                                      bool allowPollingFallback = true);
    void beginPulseCounter(pcnt_unit_handle_t unit = NULL, bool countBothEdges = false,
                           uint16_t filter_us = 0);
    void useSoftwareCounter();
    esp_err_t useSoftwareCounterChecked();

private:
    uint8_t _pin;
    uint8_t _mode;
    bool _activeHigh;
    gpio_pullup_t _pullUp;
    gpio_pulldown_t _pullDown;
    bool _validConfig;
    bool _configured;
    bool _gpioIsrRegistered;
    portMUX_TYPE _inputMux = portMUX_INITIALIZER_UNLOCKED;
    uint32_t _debounceDelayMs;
    bool _status;
    bool _defaultStatus;
    int _lastRawLevel;
    int64_t _lastDebounceUs;

    uint64_t _eventCount;
    int64_t _eventStartUs;
    uint64_t _eventLengthMs;
    int64_t _nextRepeatUs;

    uint64_t _cycleStartUs;
    uint64_t _lastCycleMs;
    bool _cycleStarted;
    bool _cycleSawInactive;

    int _lastMachineState;
    int64_t _lastStateChangeUs;
    uint64_t _runTotalMs;
    uint64_t _stopTotalMs;

    pcnt_unit_handle_t _pcntUnit;
    pcnt_channel_handle_t _pcntChannel;
    bool _pcntInitialized;
    bool _ownsPcntUnit;
    bool _pcntUnitEnabled;
    bool _pcntUnitStarted;
    esp_err_t _lastCounterError;

    bool _usingSoftwareCounter;
    WAZEH_CycleTime_Edge _edge;
    uint64_t _softwarePulseCount;
    uint64_t _pulseCountOffset;
    uint64_t _lastPulseSampleCount;
    int64_t _lastPulseSampleUs;
    float _pulseRateHz;
    uint64_t _lastRateSampleCount;
    int64_t _lastRateSampleUs;
    int64_t _pulseBucketSecond[60];
    uint64_t _pulseBuckets[60];

    WAZEH_CycleTime_Event_Callback _legacyCallback;
    WAZEH_CycleTime_TypedEventCallback _typedCallback;

    esp_err_t releasePulseCounter();
    esp_err_t releaseInputInterrupt();
    static void gpioIsrHandler(void *arg);
    esp_err_t readBackendCount(uint64_t *count);
    uint64_t updatePulseStatistics(uint64_t count, int64_t nowUs);
    void updateMachineMeasurements(bool active, int64_t nowUs);
    WAZEH_CycleTime_Event process(unsigned long debounceDelay, unsigned long repeatDelay,
                                  const WAZEH_CycleTime_Event_Callback &legacyCallback,
                                  const WAZEH_CycleTime_TypedEventCallback &typedCallback);
};

#endif
