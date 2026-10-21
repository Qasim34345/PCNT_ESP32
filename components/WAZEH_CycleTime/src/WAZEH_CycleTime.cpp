#include "WAZEH_CycleTime.h"

#include <utility>

#include "esp_log.h"

#if WAZEH_CYCLETIME_DEBUG_ENABLED
namespace {
ChronoLogger cycleTimeLoggerInstance("WAZEH_CycleTime", WAZEH_CYCLETIME_LOG_LEVEL);
}
ChronoLogger *wazehCycleTimeLogger = &cycleTimeLoggerInstance;
#endif

namespace {
constexpr char TAG[] = "WAZEH_CycleTime";
}

WAZEH_CycleTime::WAZEH_CycleTime(
    uint8_t *pins,
    size_t pinCount,
    uint8_t mode,
    WAZEH_CycleTime_Event_Callback callback)
    : delay(WAZEH_CYCLETIME_DEBOUNCE_DELAY),
      repeat(WAZEH_CYCLETIME_REPEAT_DELAY),
      callback(std::move(callback))
{
    if (pins == nullptr && pinCount != 0) {
        ESP_LOGE(TAG, "Cannot configure channels from a null pin list");
        return;
    }
    configureChannels(pins, mode, pinCount);
}

void WAZEH_CycleTime::configureChannels(uint8_t *pins, uint8_t mode, size_t pinCount)
{
    channels.reserve(pinCount);
    for (size_t i = 0; i < pinCount; ++i) {
        if (getChannel(pins[i]) != nullptr) {
            ESP_LOGE(TAG, "Duplicate GPIO%u in channel list", pins[i]);
            continue;
        }
        auto channel = std::make_unique<WAZEH_Channel>(pins[i], mode);
        const esp_err_t err = channel->configureChecked();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "GPIO%u configuration failed: %s", pins[i], esp_err_to_name(err));
            continue;
        }
        channels.push_back(std::move(channel));
    }
}

esp_err_t WAZEH_CycleTime::addChannel(const WAZEH_CycleTime_ChannelConfig &config)
{
    if (!GPIO_IS_VALID_GPIO(config.pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (getChannel(static_cast<uint8_t>(config.pin)) != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    auto channel = std::make_unique<WAZEH_Channel>(config);
    const esp_err_t err = channel->configureChecked();
    if (err != ESP_OK) {
        return err;
    }
    channels.push_back(std::move(channel));
    return ESP_OK;
}

esp_err_t WAZEH_CycleTime::deinit()
{
    esp_err_t firstError = ESP_OK;
    for (auto &channel : channels) {
        const esp_err_t err = channel->deinit();
        if (err != ESP_OK && firstError == ESP_OK) {
            firstError = err;
        }
    }
    if (firstError != ESP_OK) {
        return firstError;
    }
    channels.clear();
    return ESP_OK;
}

void WAZEH_CycleTime::setTypedEventCallback(WAZEH_CycleTime_TypedEventCallback newCallback)
{
    typedCallback = std::move(newCallback);
}

void WAZEH_CycleTime::setDebounceDelay(unsigned long value)
{
    delay = value;
    for (auto &channel : channels) {
        channel->setDebounceDelay(value > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(value));
    }
}

WAZEH_CycleTime_Event WAZEH_CycleTime::loop()
{
    WAZEH_CycleTime_Event lastEvent = CYCLETIME_EVENT_NONE;
    for (auto &channel : channels) {
        const WAZEH_CycleTime_Event event = channel->loop(
            channel->getDebounceDelay(), repeat, callback, typedCallback);
        if (event != CYCLETIME_EVENT_NONE) {
            lastEvent = event;
        }
    }
    return lastEvent;
}

WAZEH_Channel *WAZEH_CycleTime::getChannel(uint8_t pin)
{
    for (auto &channel : channels) {
        if (channel->getPin() == pin) {
            return channel.get();
        }
    }
    return nullptr;
}

bool WAZEH_CycleTime::pressed(uint8_t pin)
{
    WAZEH_Channel *channel = getChannel(pin);
    return channel != nullptr && channel->pressed();
}

unsigned long WAZEH_CycleTime::getEventLength(uint8_t pin)
{
    WAZEH_Channel *channel = getChannel(pin);
    return channel != nullptr ? channel->getEventLength() : 0;
}

unsigned long WAZEH_CycleTime::getEventCount(uint8_t pin)
{
    WAZEH_Channel *channel = getChannel(pin);
    return channel != nullptr ? channel->getEventCount() : 0;
}

unsigned long WAZEH_CycleTime::calculateCycleTime(uint8_t pin)
{
    WAZEH_Channel *channel = getChannel(pin);
    return channel != nullptr ? channel->calculateCycleTime() : 0;
}

unsigned long WAZEH_CycleTime::calculateRunTime(uint8_t pin)
{
    WAZEH_Channel *channel = getChannel(pin);
    return channel != nullptr ? channel->calculateRunTime() : 0;
}

unsigned long WAZEH_CycleTime::calculateStopTime(uint8_t pin)
{
    WAZEH_Channel *channel = getChannel(pin);
    return channel != nullptr ? channel->calculateStopTime() : 0;
}

WAZEH_CycleTime_RunStopResult WAZEH_CycleTime::calculateRunStopTime(uint8_t pin)
{
    WAZEH_Channel *channel = getChannel(pin);
    return channel != nullptr ? channel->calculateRunStopTime()
                              : WAZEH_CycleTime_RunStopResult{0, 0};
}

float WAZEH_CycleTime::calculatePulseRate(uint8_t pin)
{
    WAZEH_Channel *channel = getChannel(pin);
    return channel != nullptr ? channel->calculatePulseRate() : 0.0f;
}

long WAZEH_CycleTime::calculatePulseCount(uint8_t pin)
{
    WAZEH_Channel *channel = getChannel(pin);
    return channel != nullptr ? channel->calculatePulseCount() : -1;
}

long WAZEH_CycleTime::getRawPulseCount(uint8_t pin)
{
    WAZEH_Channel *channel = getChannel(pin);
    return channel != nullptr ? channel->getRawPulseCount() : -1;
}

long WAZEH_CycleTime::getPulseCountLastMinute(uint8_t pin)
{
    WAZEH_Channel *channel = getChannel(pin);
    return channel != nullptr ? channel->getPulseCountLastMinute() : -1;
}

void WAZEH_CycleTime::resetPulseCount(uint8_t pin)
{
    WAZEH_Channel *channel = getChannel(pin);
    if (channel != nullptr) {
        channel->resetPulseCount();
    }
}

esp_err_t WAZEH_CycleTime::resetPulseCountChecked(uint8_t pin)
{
    WAZEH_Channel *channel = getChannel(pin);
    return channel != nullptr ? channel->resetPulseCountChecked() : ESP_ERR_NOT_FOUND;
}

void WAZEH_CycleTime::initPulseCounterByPin(
    uint8_t pin,
    bool countBothEdges,
    uint16_t filterUs)
{
    WAZEH_Channel *channel = getChannel(pin);
    if (channel != nullptr) {
        channel->initPulseCounter(countBothEdges, filterUs);
    }
}

esp_err_t WAZEH_CycleTime::initPulseCounterByPinChecked(
    uint8_t pin,
    WAZEH_CycleTime_Edge edge,
    uint32_t filterNs,
    bool allowPollingFallback)
{
    WAZEH_Channel *channel = getChannel(pin);
    return channel != nullptr
        ? channel->initPulseCounterChecked(edge, filterNs, allowPollingFallback)
        : ESP_ERR_NOT_FOUND;
}

esp_err_t WAZEH_CycleTime::getPulseCount(uint8_t pin, uint64_t *count)
{
    WAZEH_Channel *channel = getChannel(pin);
    return channel != nullptr ? channel->getPulseCount(count) : ESP_ERR_NOT_FOUND;
}
