#ifndef WAZEH_CYCLETIME_H
#define WAZEH_CYCLETIME_H

#include "WAZEH_Channel.h"
#include "WAZEH_CycleTime_Config.h"
#include <array>
#include <memory>

class WAZEH_CycleTime {
public:
    WAZEH_CycleTime() = default;
    WAZEH_CycleTime(
        uint8_t *pins,
        size_t pinCount                         = 1,
        uint8_t mode                            = WAZEH_CycleTime_InputMode::CYCLETIME_INPUT_MODE_PUSHBUTTON,
        WAZEH_CycleTime_Event_Callback callback = nullptr
    );

    WAZEH_CycleTime(const WAZEH_CycleTime &) = delete;
    WAZEH_CycleTime &operator=(const WAZEH_CycleTime &) = delete;
    WAZEH_CycleTime(WAZEH_CycleTime &&) = delete;
    WAZEH_CycleTime &operator=(WAZEH_CycleTime &&) = delete;

    esp_err_t addChannel(const WAZEH_CycleTime_ChannelConfig &config);
    esp_err_t deinit();
    void setTypedEventCallback(WAZEH_CycleTime_TypedEventCallback callback);
    WAZEH_CycleTime_Event loop();

    // Configuration. setDebounceDelay updates currently configured channels;
    // typed channel configurations retain their own debounce_ms value.
    void setRepeatDelay(unsigned long value) { repeat = value; }
    void setDebounceDelay(unsigned long value);

    // Channel Accessors (by pin)
    bool pressed(uint8_t pin);
    unsigned long getEventLength(uint8_t pin);
    unsigned long getEventCount(uint8_t pin);

    unsigned long calculateRunTime(uint8_t pin);
    unsigned long calculateStopTime(uint8_t pin);
    unsigned long calculateCycleTime(uint8_t pin);
    WAZEH_CycleTime_RunStopResult calculateRunStopTime(uint8_t pin);

    long getRawPulseCount(uint8_t pin);
    float calculatePulseRate(uint8_t pin);
    long calculatePulseCount(uint8_t pin);
    long getPulseCountLastMinute(uint8_t pin);

    void resetPulseCount(uint8_t pin);
    esp_err_t resetPulseCountChecked(uint8_t pin);
    void initPulseCounterByPin(uint8_t pin, bool countBothEdges = false, uint16_t filter_us = 0);
    esp_err_t initPulseCounterByPinChecked(uint8_t pin, WAZEH_CycleTime_Edge edge,
                                           uint32_t filter_ns,
                                           bool allowPollingFallback = true);
    esp_err_t getPulseCount(uint8_t pin, uint64_t *count);

    WAZEH_Channel* getChannel(uint8_t pin);

private:
    // Safe defaults are required when the default constructor + addChannel() is used.
    unsigned long                   repeat = WAZEH_CYCLETIME_REPEAT_DELAY;
    std::array<std::unique_ptr<WAZEH_Channel>, WAZEH_CYCLETIME_MAX_CHANNELS> channels{};
    size_t channelCount = 0;
    WAZEH_CycleTime_Event_Callback  callback;
    WAZEH_CycleTime_TypedEventCallback typedCallback;

    void configureChannels(uint8_t *pins, uint8_t mode, size_t pinCount);
};

#endif // WAZEH_CYCLETIME_H