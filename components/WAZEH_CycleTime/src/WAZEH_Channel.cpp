#include "WAZEH_Channel.h"

#include <inttypes.h>
#include <limits.h>
#include <soc/soc_caps.h>

#include "esp_log.h"

namespace {

constexpr char TAG[] = "WAZEH_CycleTime";
constexpr int PCNT_HIGH_LIMIT = 32767;
constexpr int PCNT_LOW_LIMIT = -32768;

gpio_pullup_t pullUpFor(WAZEH_CycleTime_Pull pull)
{
    return pull == WAZEH_CycleTime_Pull::Up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
}

gpio_pulldown_t pullDownFor(WAZEH_CycleTime_Pull pull)
{
    return pull == WAZEH_CycleTime_Pull::Down ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
}

esp_err_t ensureGpioIsrService()
{
    static bool serviceReady = false;
    if (serviceReady) {
        return ESP_OK;
    }

    const esp_err_t err = gpio_install_isr_service(0);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        serviceReady = true;
        return ESP_OK;
    }
    return err;
}

WAZEH_CycleTime_ChannelConfig legacyConfig(uint8_t pin, uint8_t mode)
{
    WAZEH_CycleTime_ChannelConfig config{};
    config.pin = static_cast<gpio_num_t>(pin);
    config.input_mode = (mode & CYCLETIME_INPUT_MODE_SWITCH)
        ? CYCLETIME_INPUT_MODE_SWITCH
        : CYCLETIME_INPUT_MODE_PUSHBUTTON;
    config.active_level = (mode & WAZEH_CYCLETIME_CHANNEL_DEFAULT_HIGH)
        ? WAZEH_CycleTime_ActiveLevel::Low
        : WAZEH_CycleTime_ActiveLevel::High;
    if (mode & WAZEH_CYCLETIME_CHANNEL_PULLUP) {
        config.pull = WAZEH_CycleTime_Pull::Up;
    } else if (!(mode & WAZEH_CYCLETIME_CHANNEL_DEFAULT_HIGH)) {
        config.pull = WAZEH_CycleTime_Pull::Down;
    }
    return config;
}

uint64_t saturatingAdd(uint64_t a, uint64_t b)
{
    return UINT64_MAX - a < b ? UINT64_MAX : a + b;
}

unsigned long toUnsignedLong(uint64_t value)
{
    return value > ULONG_MAX ? ULONG_MAX : static_cast<unsigned long>(value);
}

long toLong(uint64_t value)
{
    return value > static_cast<uint64_t>(LONG_MAX) ? LONG_MAX : static_cast<long>(value);
}

} // namespace

WAZEH_Channel::WAZEH_Channel(uint8_t pin, uint8_t mode)
    : WAZEH_Channel(legacyConfig(pin, mode))
{
    _mode = mode;
}

WAZEH_Channel::WAZEH_Channel(const WAZEH_CycleTime_ChannelConfig &config)
    : _pin(static_cast<uint8_t>(config.pin)),
      _mode(static_cast<uint8_t>(config.input_mode)),
      _activeHigh(config.active_level == WAZEH_CycleTime_ActiveLevel::High),
      _pullUp(pullUpFor(config.pull)),
      _pullDown(pullDownFor(config.pull)),
      _validConfig(
          (config.input_mode == CYCLETIME_INPUT_MODE_PUSHBUTTON ||
           config.input_mode == CYCLETIME_INPUT_MODE_SWITCH) &&
          (config.active_level == WAZEH_CycleTime_ActiveLevel::Low ||
           config.active_level == WAZEH_CycleTime_ActiveLevel::High) &&
          (config.pull == WAZEH_CycleTime_Pull::None ||
           config.pull == WAZEH_CycleTime_Pull::Up ||
           config.pull == WAZEH_CycleTime_Pull::Down)),
      _configured(false),
      _gpioIsrRegistered(false),
      _debounceDelayMs(config.debounce_ms),
      _status(false),
      _defaultStatus(false),
      _lastRawLevel(-1),
      _lastDebounceUs(0),
      _eventCount(0),
      _eventStartUs(0),
      _eventLengthMs(0),
      _nextRepeatUs(0),
      _cycleStartUs(0),
      _lastCycleMs(0),
      _cycleStarted(false),
      _cycleSawInactive(false),
      _lastMachineState(-1),
      _lastStateChangeUs(0),
      _runTotalMs(0),
      _stopTotalMs(0),
      _pcntUnit(nullptr),
      _pcntChannel(nullptr),
      _pcntInitialized(false),
      _ownsPcntUnit(false),
      _pcntUnitEnabled(false),
      _pcntUnitStarted(false),
      _lastCounterError(ESP_OK),
      _usingSoftwareCounter(false),
      _edge(WAZEH_CycleTime_Edge::Rising),
      _softwarePulseCount(0),
      _pulseCountOffset(0),
      _lastKnownTotalCount(0),
      _hasLastKnownTotalCount(false),
      _lastPulseSampleCount(0),
      _lastPulseSampleUs(0),
      _pulseRateHz(0.0f),
      _lastRateSampleCount(0),
      _lastRateSampleUs(0)
{
    _defaultStatus = !_activeHigh;
    for (size_t i = 0; i < 60; ++i) {
        _pulseBucketSecond[i] = -1;
        _pulseBuckets[i] = 0;
    }
}

WAZEH_Channel::~WAZEH_Channel()
{
    const esp_err_t err = deinit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d cleanup failed: %s", _pin, esp_err_to_name(err));
    }
}

void WAZEH_Channel::configure()
{
    const esp_err_t err = configureChecked();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d configuration failed: %s", _pin, esp_err_to_name(err));
    }
}

esp_err_t WAZEH_Channel::configureChecked()
{
    if (_configured) {
        return ESP_OK;
    }
    if (!GPIO_IS_VALID_GPIO(static_cast<gpio_num_t>(_pin))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!_validConfig) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << _pin;
    config.mode = GPIO_MODE_INPUT;
    config.intr_type = GPIO_INTR_DISABLE;
    config.pull_up_en = _pullUp;
    config.pull_down_en = _pullDown;

    const esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        return err;
    }

    const gpio_num_t pin = static_cast<gpio_num_t>(_pin);
    const auto rollbackPinConfiguration = [this, pin]() -> esp_err_t {
        esp_err_t cleanupErr = ESP_OK;
        if (_gpioIsrRegistered) {
            cleanupErr = releaseInputInterrupt();
        } else {
            cleanupErr = gpio_set_intr_type(pin, GPIO_INTR_DISABLE);
        }
        if (cleanupErr == ESP_OK) {
            const esp_err_t resetErr = gpio_reset_pin(pin);
            if (resetErr != ESP_OK) cleanupErr = resetErr;
        }
        _configured = false;
        return cleanupErr;
    };
    const int level = gpio_get_level(pin);
    if (level < 0) {
        const esp_err_t cleanupErr = rollbackPinConfiguration();
        return cleanupErr == ESP_OK ? ESP_FAIL : cleanupErr;
    }

    const int64_t nowUs = esp_timer_get_time();
    _status = level != 0;
    _lastMachineState = (_status != _defaultStatus) ? 1 : 0;
    _lastStateChangeUs = nowUs;
    _eventStartUs = (_status != _defaultStatus) ? nowUs : 0;
    _cycleStarted = (_status != _defaultStatus);
    _cycleStartUs = _cycleStarted ? static_cast<uint64_t>(nowUs) : 0;
    _cycleSawInactive = false;

    esp_err_t isrErr = ensureGpioIsrService();
    if (isrErr != ESP_OK) {
        const esp_err_t cleanupErr = rollbackPinConfiguration();
        return cleanupErr == ESP_OK ? isrErr : cleanupErr;
    }
    isrErr = gpio_set_intr_type(pin, GPIO_INTR_ANYEDGE);
    if (isrErr != ESP_OK) {
        const esp_err_t cleanupErr = rollbackPinConfiguration();
        return cleanupErr == ESP_OK ? isrErr : cleanupErr;
    }
    isrErr = gpio_isr_handler_add(pin, gpioIsrHandler, this);
    if (isrErr != ESP_OK) {
        const esp_err_t cleanupErr = rollbackPinConfiguration();
        return cleanupErr == ESP_OK ? isrErr : cleanupErr;
    }
    _gpioIsrRegistered = true;
    const int initialLevel = gpio_get_level(pin);
    if (initialLevel < 0) {
        const esp_err_t cleanupErr = rollbackPinConfiguration();
        return cleanupErr == ESP_OK ? ESP_FAIL : cleanupErr;
    }
    const int64_t interruptStartUs = esp_timer_get_time();
    _status = initialLevel != 0;
    _lastMachineState = (_status != _defaultStatus) ? 1 : 0;
    _lastStateChangeUs = interruptStartUs;
    _eventStartUs = (_status != _defaultStatus) ? interruptStartUs : 0;
    _cycleStarted = (_status != _defaultStatus);
    _cycleStartUs = _cycleStarted ? static_cast<uint64_t>(interruptStartUs) : 0;
    _cycleSawInactive = false;
    portENTER_CRITICAL(&_inputMux);
    _lastRawLevel = initialLevel;
    _lastDebounceUs = interruptStartUs;
    portEXIT_CRITICAL(&_inputMux);
    isrErr = gpio_intr_enable(pin);
    if (isrErr != ESP_OK) {
        const esp_err_t cleanupErr = rollbackPinConfiguration();
        return cleanupErr == ESP_OK ? isrErr : cleanupErr;
    }
    _configured = true;
    return ESP_OK;
}

esp_err_t WAZEH_Channel::deinit()
{
    const esp_err_t inputErr = releaseInputInterrupt();
    if (inputErr != ESP_OK) {
        return inputErr;
    }
    const esp_err_t err = releasePulseCounter();
    if (err == ESP_OK) {
        _configured = false;
        portENTER_CRITICAL(&_inputMux);
        _usingSoftwareCounter = false;
        portEXIT_CRITICAL(&_inputMux);
    }
    return err;
}

esp_err_t WAZEH_Channel::releaseInputInterrupt()
{
    if (!_gpioIsrRegistered) {
        return ESP_OK;
    }

    const gpio_num_t pin = static_cast<gpio_num_t>(_pin);
    esp_err_t err = gpio_intr_disable(pin);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_isr_handler_remove(pin);
    if (err != ESP_OK) {
        return err;
    }
    _gpioIsrRegistered = false;
    return gpio_set_intr_type(pin, GPIO_INTR_DISABLE);
}

void WAZEH_Channel::gpioIsrHandler(void *arg)
{
    auto *channel = static_cast<WAZEH_Channel *>(arg);
    const int level = gpio_get_level(static_cast<gpio_num_t>(channel->_pin));
    if (level < 0) {
        return;
    }

    const int64_t nowUs = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&channel->_inputMux);
    const int previousLevel = channel->_lastRawLevel;
    if (level != previousLevel) {
        channel->_lastRawLevel = level;
        channel->_lastDebounceUs = nowUs;
        if (channel->_usingSoftwareCounter && previousLevel >= 0) {
            const bool rising = previousLevel == 0 && level == 1;
            const bool falling = previousLevel == 1 && level == 0;
            const bool selected =
                (channel->_edge == WAZEH_CycleTime_Edge::Rising && rising) ||
                (channel->_edge == WAZEH_CycleTime_Edge::Falling && falling) ||
                (channel->_edge == WAZEH_CycleTime_Edge::Both && (rising || falling));
            if (selected && channel->_softwarePulseCount != UINT64_MAX) {
                ++channel->_softwarePulseCount;
            }
        }
    }
    portEXIT_CRITICAL_ISR(&channel->_inputMux);
}

bool WAZEH_Channel::pressed() const
{
    return _status != _defaultStatus;
}

unsigned long WAZEH_Channel::getEventLength() const
{
    return toUnsignedLong(_eventLengthMs);
}

unsigned long WAZEH_Channel::getEventCount() const
{
    return toUnsignedLong(_eventCount);
}

WAZEH_CycleTime_Event WAZEH_Channel::loop(
    unsigned long debounceDelay,
    unsigned long repeatDelay,
    WAZEH_CycleTime_Event_Callback callback)
{
    return process(debounceDelay, repeatDelay, callback, {});
}

WAZEH_CycleTime_Event WAZEH_Channel::loop(
    unsigned long debounceDelay,
    unsigned long repeatDelay,
    const WAZEH_CycleTime_TypedEventCallback &callback)
{
    return process(debounceDelay, repeatDelay, {}, callback);
}

WAZEH_CycleTime_Event WAZEH_Channel::loop(
    unsigned long debounceDelay,
    unsigned long repeatDelay,
    const WAZEH_CycleTime_Event_Callback &legacyCallback,
    const WAZEH_CycleTime_TypedEventCallback &typedCallback)
{
    return process(debounceDelay, repeatDelay, legacyCallback, typedCallback);
}

WAZEH_CycleTime_Event WAZEH_Channel::process(
    unsigned long debounceDelay,
    unsigned long repeatDelay,
    const WAZEH_CycleTime_Event_Callback &legacyCallback,
    const WAZEH_CycleTime_TypedEventCallback &typedCallback)
{
    if (!_configured) {
        return CYCLETIME_EVENT_NONE;
    }

    int current = -1;
    int64_t lastEdgeUs = 0;
    portENTER_CRITICAL(&_inputMux);
    current = _lastRawLevel;
    lastEdgeUs = _lastDebounceUs;
    portEXIT_CRITICAL(&_inputMux);
    if (current < 0) {
        return CYCLETIME_EVENT_NONE;
    }
    const int64_t nowUs = esp_timer_get_time();

    WAZEH_CycleTime_Event event = CYCLETIME_EVENT_NONE;
    if (nowUs - lastEdgeUs >= static_cast<int64_t>(debounceDelay) * 1000 &&
        current != static_cast<int>(_status)) {
        _status = current != 0;
        const bool active = pressed();
        updateMachineMeasurements(active, nowUs);

        if (_mode & CYCLETIME_INPUT_MODE_SWITCH) {
            event = CYCLETIME_EVENT_CHANGED;
            if (_eventCount != UINT64_MAX) {
                ++_eventCount;
            }
        } else if (active) {
            event = CYCLETIME_EVENT_PRESSED;
            if (_eventCount != UINT64_MAX) {
                ++_eventCount;
            }
            _eventStartUs = nowUs;
            _eventLengthMs = 0;
            _nextRepeatUs = repeatDelay > 0
                ? nowUs + static_cast<int64_t>(repeatDelay) * 1000
                : 0;
        } else {
            event = CYCLETIME_EVENT_RELEASED;
            _eventLengthMs = nowUs >= _eventStartUs
                ? static_cast<uint64_t>(nowUs - _eventStartUs) / 1000
                : 0;
            _nextRepeatUs = 0;
        }
    } else if (!(_mode & CYCLETIME_INPUT_MODE_SWITCH) && pressed() &&
               repeatDelay > 0 && _nextRepeatUs > 0 && nowUs >= _nextRepeatUs) {
        event = CYCLETIME_EVENT_REPEAT;
        _eventLengthMs = nowUs >= _eventStartUs
            ? static_cast<uint64_t>(nowUs - _eventStartUs) / 1000
            : 0;
        _nextRepeatUs = nowUs + static_cast<int64_t>(repeatDelay) * 1000;
    }

    if (event != CYCLETIME_EVENT_NONE) {
        const WAZEH_CycleTime_EventData data{
            static_cast<gpio_num_t>(_pin), event, _eventCount, _eventLengthMs, nowUs
        };
        if (typedCallback) {
            typedCallback(data);
        }
        if (legacyCallback) {
            const uint8_t legacyCount = _eventCount > UINT8_MAX
                ? UINT8_MAX
                : static_cast<uint8_t>(_eventCount);
            const uint16_t legacyLength = _eventLengthMs > UINT16_MAX
                ? UINT16_MAX
                : static_cast<uint16_t>(_eventLengthMs);
            legacyCallback(_pin, static_cast<uint8_t>(event), legacyCount, legacyLength);
        }
    }
    return event;
}

void WAZEH_Channel::updateMachineMeasurements(bool active, int64_t nowUs)
{
    if (_lastMachineState >= 0 && static_cast<int>(active) != _lastMachineState) {
        const uint64_t elapsedMs = static_cast<uint64_t>(nowUs - _lastStateChangeUs) / 1000;
        if (_lastMachineState != 0) {
            _runTotalMs = saturatingAdd(_runTotalMs, elapsedMs);
        } else {
            _stopTotalMs = saturatingAdd(_stopTotalMs, elapsedMs);
        }
        _lastMachineState = active ? 1 : 0;
        _lastStateChangeUs = nowUs;
    }

    if (active) {
        if (!_cycleStarted) {
            _cycleStartUs = static_cast<uint64_t>(nowUs);
            _cycleStarted = true;
            _cycleSawInactive = false;
        } else if (_cycleSawInactive) {
            _lastCycleMs = static_cast<uint64_t>(nowUs - static_cast<int64_t>(_cycleStartUs)) / 1000;
            _cycleStartUs = static_cast<uint64_t>(nowUs);
            _cycleSawInactive = false;
        }
    } else if (_cycleStarted) {
        _cycleSawInactive = true;
    }
}

unsigned long WAZEH_Channel::calculateRunTime()
{
    return calculateRunStopTime().run_time;
}

unsigned long WAZEH_Channel::calculateStopTime()
{
    return calculateRunStopTime().stop_time;
}

WAZEH_CycleTime_RunStopResult WAZEH_Channel::calculateRunStopTime()
{
    const int64_t nowUs = esp_timer_get_time();
    uint64_t runMs = _runTotalMs;
    uint64_t stopMs = _stopTotalMs;
    if (_lastMachineState >= 0 && nowUs >= _lastStateChangeUs) {
        const uint64_t elapsedMs = static_cast<uint64_t>(nowUs - _lastStateChangeUs) / 1000;
        if (_lastMachineState != 0) {
            runMs = saturatingAdd(runMs, elapsedMs);
        } else {
            stopMs = saturatingAdd(stopMs, elapsedMs);
        }
    }
    return {toUnsignedLong(runMs), toUnsignedLong(stopMs)};
}

unsigned long WAZEH_Channel::calculateCycleTime()
{
    return toUnsignedLong(_lastCycleMs);
}

esp_err_t WAZEH_Channel::releasePulseCounter()
{
    // Once teardown begins, do not advertise the unit as a healthy backend until
    // a subsequent initialization succeeds. Handles remain tracked for retry.
    _pcntInitialized = false;
    esp_err_t firstError = ESP_OK;
#if SOC_PCNT_SUPPORTED
    if (_pcntUnitStarted && _pcntUnit != nullptr && _ownsPcntUnit) {
        const esp_err_t err = pcnt_unit_stop(_pcntUnit);
        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
            // INVALID_STATE can mean the driver is already stopped (for example,
            // after an earlier partial failure). Continue cleanup idempotently.
            _pcntUnitStarted = false;
        } else if (firstError == ESP_OK) {
            firstError = err;
        }
    }
    if (_pcntUnitEnabled && !_pcntUnitStarted && _pcntUnit != nullptr && _ownsPcntUnit) {
        const esp_err_t err = pcnt_unit_disable(_pcntUnit);
        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
            _pcntUnitEnabled = false;
        } else if (firstError == ESP_OK) {
            firstError = err;
        }
    }
    if (_pcntChannel != nullptr && (!_ownsPcntUnit || !_pcntUnitEnabled)) {
        const esp_err_t err = pcnt_del_channel(_pcntChannel);
        if (err == ESP_OK) {
            _pcntChannel = nullptr;
        } else if (firstError == ESP_OK) {
            firstError = err;
        }
    }
    if (_pcntUnit != nullptr && _ownsPcntUnit && _pcntChannel == nullptr &&
        !_pcntUnitEnabled && !_pcntUnitStarted) {
        const esp_err_t err = pcnt_del_unit(_pcntUnit);
        if (err == ESP_OK) {
            _pcntUnit = nullptr;
            _ownsPcntUnit = false;
        } else if (firstError == ESP_OK) {
            firstError = err;
        }
    }
    // The component owns an external unit's channel, but not the unit itself.
    // Once its channel is gone, clear our borrowed handle so a later owned-unit
    // initialization cannot overwrite a stale external handle.
    if (_pcntUnit != nullptr && !_ownsPcntUnit && _pcntChannel == nullptr) {
        _pcntUnit = nullptr;
    }
#endif
    if (_pcntChannel == nullptr && (_pcntUnit == nullptr || !_ownsPcntUnit)) {
        _pcntInitialized = false;
        _pcntUnitEnabled = false;
        _pcntUnitStarted = false;
    }
    return firstError;
}

esp_err_t WAZEH_Channel::readBackendCount(uint64_t *count)
{
    if (count == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!_configured) {
        return ESP_ERR_INVALID_STATE;
    }

    if (_usingSoftwareCounter) {
        portENTER_CRITICAL(&_inputMux);
        *count = _softwarePulseCount;
        portEXIT_CRITICAL(&_inputMux);
        return ESP_OK;
    }

    if (!_pcntInitialized || _pcntUnit == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
#if SOC_PCNT_SUPPORTED
    int value = 0;
    const esp_err_t err = pcnt_unit_get_count(_pcntUnit, &value);
    if (err != ESP_OK) {
        return err;
    }
    if (value < 0) {
        return ESP_FAIL;
    }
    *count = static_cast<uint64_t>(value);
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

uint64_t WAZEH_Channel::updatePulseStatistics(uint64_t count, int64_t nowUs)
{
    const int64_t second = nowUs / 1000000;
    const size_t slot = static_cast<size_t>(second % 60);
    if (_pulseBucketSecond[slot] != second) {
        _pulseBucketSecond[slot] = second;
        _pulseBuckets[slot] = 0;
    }

    if (_lastPulseSampleUs > 0 && count >= _lastPulseSampleCount) {
        const uint64_t delta = count - _lastPulseSampleCount;
        if (UINT64_MAX - _pulseBuckets[slot] < delta) {
            _pulseBuckets[slot] = UINT64_MAX;
        } else {
            _pulseBuckets[slot] += delta;
        }
    }
    _lastPulseSampleCount = count;
    _lastPulseSampleUs = nowUs;

    uint64_t sum = 0;
    const int64_t oldestSecond = second - 59;
    for (size_t i = 0; i < 60; ++i) {
        if (_pulseBucketSecond[i] >= oldestSecond && _pulseBucketSecond[i] <= second) {
            if (UINT64_MAX - sum < _pulseBuckets[i]) {
                return UINT64_MAX;
            }
            sum += _pulseBuckets[i];
        }
    }
    return sum;
}

esp_err_t WAZEH_Channel::getPulseCount(uint64_t *count)
{
    if (count == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    uint64_t backendCount = 0;
    const esp_err_t err = readBackendCount(&backendCount);
    if (err != ESP_OK) {
        _lastCounterError = err;
        return err;
    }
    if (UINT64_MAX - _pulseCountOffset < backendCount) {
        *count = UINT64_MAX;
    } else {
        *count = _pulseCountOffset + backendCount;
    }
    _lastKnownTotalCount = *count;
    _hasLastKnownTotalCount = true;
    updatePulseStatistics(*count, esp_timer_get_time());
    _lastCounterError = ESP_OK;
    return ESP_OK;
}

long WAZEH_Channel::getRawPulseCount()
{
    uint64_t count = 0;
    const esp_err_t err = readBackendCount(&count);
    if (err != ESP_OK) {
        _lastCounterError = err;
        return -1;
    }
    _lastCounterError = ESP_OK;
    return toLong(count);
}

WAZEH_CycleTime_CounterBackend WAZEH_Channel::getCounterBackend() const
{
    if (_pcntInitialized) return WAZEH_CycleTime_CounterBackend::HardwarePcnt;
    if (_usingSoftwareCounter) return WAZEH_CycleTime_CounterBackend::Interrupt;
    return WAZEH_CycleTime_CounterBackend::Unavailable;
}

float WAZEH_Channel::calculatePulseRate()
{
    uint64_t count = 0;
    if (getPulseCount(&count) != ESP_OK) {
        return 0.0f;
    }
    const int64_t nowUs = esp_timer_get_time();
    if (_lastRateSampleUs > 0 && nowUs > _lastRateSampleUs &&
        count >= _lastRateSampleCount) {
        const uint64_t delta = count - _lastRateSampleCount;
        _pulseRateHz = static_cast<float>(delta) * 1000000.0f /
                       static_cast<float>(nowUs - _lastRateSampleUs);
    }
    _lastRateSampleCount = count;
    _lastRateSampleUs = nowUs;
    return _pulseRateHz;
}

long WAZEH_Channel::calculatePulseCount()
{
    uint64_t count = 0;
    return getPulseCount(&count) == ESP_OK ? toLong(count) : -1;
}

long WAZEH_Channel::getPulseCountLastMinute()
{
    uint64_t count = 0;
    if (getPulseCount(&count) != ESP_OK) {
        return -1;
    }

    // getPulseCount() already records this sample. Sum the current window without
    // recording a second sample that could land in a different second bucket.
    const int64_t nowUs = esp_timer_get_time();
    const int64_t currentSecond = nowUs / 1000000;
    const int64_t oldestSecond = currentSecond - 59;
    uint64_t sum = 0;
    for (size_t i = 0; i < 60; ++i) {
        if (_pulseBucketSecond[i] >= oldestSecond &&
            _pulseBucketSecond[i] <= currentSecond) {
            if (UINT64_MAX - sum < _pulseBuckets[i]) {
                return LONG_MAX;
            }
            sum += _pulseBuckets[i];
        }
    }
    return toLong(sum);
}

esp_err_t WAZEH_Channel::resetPulseCountChecked()
{
    if (!_configured || !isCounterAvailable()) {
        _lastCounterError = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t currentTotal = 0;
    if (_usingSoftwareCounter) {
        // Snapshot and clear under the same critical section used by the ISR so
        // an edge cannot fall between the count snapshot and software reset.
        portENTER_CRITICAL(&_inputMux);
        currentTotal = saturatingAdd(_pulseCountOffset, _softwarePulseCount);
        _pulseCountOffset = currentTotal;
        _softwarePulseCount = 0;
        portEXIT_CRITICAL(&_inputMux);
    } else {
#if SOC_PCNT_SUPPORTED
        // Do not clear a borrowed PCNT unit: it may be shared with other channels.
        // This component owns only the channel attached to that unit.
        if (!_ownsPcntUnit || _pcntUnit == nullptr) {
            _lastCounterError = ESP_ERR_NOT_SUPPORTED;
            return ESP_ERR_NOT_SUPPORTED;
        }
        const bool restart = _pcntUnitStarted;
        if (restart) {
            const esp_err_t stopErr = pcnt_unit_stop(_pcntUnit);
            if (stopErr != ESP_OK && stopErr != ESP_ERR_INVALID_STATE) {
                _lastCounterError = stopErr;
                return stopErr;
            }
            // INVALID_STATE is tolerated as an already-stopped unit; the following
            // count read will still surface an unusable/disabled unit if applicable.
            _pcntUnitStarted = false;
        }
        int raw = 0;
        esp_err_t err = pcnt_unit_get_count(_pcntUnit, &raw);
        bool cleared = false;
        if (err == ESP_OK && raw >= 0) {
            currentTotal = saturatingAdd(_pulseCountOffset, static_cast<uint64_t>(raw));
            err = pcnt_unit_clear_count(_pcntUnit);
            cleared = err == ESP_OK;
            if (cleared) {
                // Publish the cumulative offset only after clear succeeds. If start
                // later fails, already-counted pulses remain represented in the total.
                _pulseCountOffset = currentTotal;
                _lastKnownTotalCount = currentTotal;
                _hasLastKnownTotalCount = true;
            }
        } else if (err == ESP_OK) {
            err = ESP_FAIL;
        }
        if (restart) {
            const esp_err_t startErr = pcnt_unit_start(_pcntUnit);
            if (startErr == ESP_OK) {
                _pcntUnitStarted = true;
            } else {
                if (err == ESP_OK) err = startErr;
                _pcntInitialized = false;
                ESP_LOGE(TAG, "GPIO%d PCNT restart after reset failed: %s",
                         _pin, esp_err_to_name(startErr));
            }
        }
        if (err != ESP_OK) {
            _lastCounterError = err;
            return err;
        }
#else
        _lastCounterError = ESP_ERR_NOT_SUPPORTED;
        return ESP_ERR_NOT_SUPPORTED;
#endif
    }

    _lastKnownTotalCount = currentTotal;
    _hasLastKnownTotalCount = true;
    const int64_t sampleTimeUs = esp_timer_get_time();
    updatePulseStatistics(currentTotal, sampleTimeUs);
    _lastPulseSampleCount = currentTotal;
    _lastPulseSampleUs = sampleTimeUs;
    _lastRateSampleCount = currentTotal;
    _lastRateSampleUs = sampleTimeUs;
    _lastCounterError = ESP_OK;
    return ESP_OK;
}

void WAZEH_Channel::resetPulseCount()
{
    const esp_err_t err = resetPulseCountChecked();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d pulse-counter reset failed: %s", _pin, esp_err_to_name(err));
    }
}

esp_err_t WAZEH_Channel::initPulseCounterChecked(
    WAZEH_CycleTime_Edge edge,
    uint32_t filterNs,
    bool allowPollingFallback)
{
    if (!_configured) {
        return ESP_ERR_INVALID_STATE;
    }
    if (edge != WAZEH_CycleTime_Edge::Rising &&
        edge != WAZEH_CycleTime_Edge::Falling &&
        edge != WAZEH_CycleTime_Edge::Both) {
        return ESP_ERR_INVALID_ARG;
    }
    if (filterNs > 1000000000U) {
        return ESP_ERR_INVALID_ARG;
    }

    // Preserve the cumulative total when reconfiguring the counter. If the
    // current backend cannot be read, retain the last known good total; the
    // resulting transition may lose pulses since that snapshot, which is logged.
    uint64_t preservedTotal = _lastKnownTotalCount;
    bool hadKnownTotal = _hasLastKnownTotalCount;
    if (isCounterAvailable()) {
        uint64_t liveTotal = 0;
        if (getPulseCount(&liveTotal) == ESP_OK) {
            preservedTotal = liveTotal;
            hadKnownTotal = true;
        } else {
            ESP_LOGW(TAG, "GPIO%d reconfiguring counter after read failure; preserving last known total only",
                     _pin);
        }
    }

    const esp_err_t cleanupErr = releasePulseCounter();
    if (cleanupErr != ESP_OK) {
        _lastCounterError = cleanupErr;
        return cleanupErr;
    }

    portENTER_CRITICAL(&_inputMux);
    _edge = edge;
    _usingSoftwareCounter = false;
    _softwarePulseCount = 0;
    portEXIT_CRITICAL(&_inputMux);
    _pulseCountOffset = hadKnownTotal ? preservedTotal : 0;
    _lastKnownTotalCount = _pulseCountOffset;
    _hasLastKnownTotalCount = hadKnownTotal;
    _lastPulseSampleCount = _pulseCountOffset;
    _lastPulseSampleUs = hadKnownTotal ? esp_timer_get_time() : 0;
    _pulseRateHz = 0.0f;
    _lastRateSampleCount = _pulseCountOffset;
    _lastRateSampleUs = 0;
    if (!hadKnownTotal) {
        for (size_t i = 0; i < 60; ++i) {
            _pulseBucketSecond[i] = -1;
            _pulseBuckets[i] = 0;
        }
    }

    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
#if SOC_PCNT_SUPPORTED
    pcnt_unit_config_t unitConfig{};
    unitConfig.low_limit = PCNT_LOW_LIMIT;
    unitConfig.high_limit = PCNT_HIGH_LIMIT;
    unitConfig.flags.accum_count = 1;

    err = pcnt_new_unit(&unitConfig, &_pcntUnit);
    if (err == ESP_OK) {
        _ownsPcntUnit = true;
        pcnt_chan_config_t channelConfig{};
        channelConfig.edge_gpio_num = static_cast<gpio_num_t>(_pin);
        channelConfig.level_gpio_num = -1;
        err = pcnt_new_channel(_pcntUnit, &channelConfig, &_pcntChannel);
    }
    if (err == ESP_OK) {
        const pcnt_channel_edge_action_t posAction =
            edge == WAZEH_CycleTime_Edge::Falling
                ? PCNT_CHANNEL_EDGE_ACTION_HOLD
                : PCNT_CHANNEL_EDGE_ACTION_INCREASE;
        const pcnt_channel_edge_action_t negAction =
            edge == WAZEH_CycleTime_Edge::Rising
                ? PCNT_CHANNEL_EDGE_ACTION_HOLD
                : PCNT_CHANNEL_EDGE_ACTION_INCREASE;
        err = pcnt_channel_set_edge_action(_pcntChannel, posAction, negAction);
    }
    if (err == ESP_OK) {
        err = pcnt_channel_set_level_action(
            _pcntChannel,
            PCNT_CHANNEL_LEVEL_ACTION_KEEP,
            PCNT_CHANNEL_LEVEL_ACTION_KEEP);
    }
    if (err == ESP_OK && filterNs > 0) {
        pcnt_glitch_filter_config_t filterConfig{};
        filterConfig.max_glitch_ns = filterNs;
        err = pcnt_unit_set_glitch_filter(_pcntUnit, &filterConfig);
    }
    if (err == ESP_OK) {
        err = pcnt_unit_add_watch_point(_pcntUnit, PCNT_LOW_LIMIT);
    }
    if (err == ESP_OK) {
        err = pcnt_unit_add_watch_point(_pcntUnit, PCNT_HIGH_LIMIT);
    }
    if (err == ESP_OK) {
        err = pcnt_unit_enable(_pcntUnit);
        _pcntUnitEnabled = err == ESP_OK;
    }
    if (err == ESP_OK) {
        err = pcnt_unit_clear_count(_pcntUnit);
    }
    if (err == ESP_OK) {
        err = pcnt_unit_start(_pcntUnit);
        _pcntUnitStarted = err == ESP_OK;
    }
    if (err == ESP_OK) {
        _pcntInitialized = true;
        portENTER_CRITICAL(&_inputMux);
        _usingSoftwareCounter = false;
        portEXIT_CRITICAL(&_inputMux);
        _lastCounterError = ESP_OK;
        return ESP_OK;
    }
#endif

    _lastCounterError = err;
    const esp_err_t rollbackErr = releasePulseCounter();
    if (rollbackErr != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d PCNT rollback failed: %s", _pin, esp_err_to_name(rollbackErr));
        _lastCounterError = rollbackErr;
        return rollbackErr;
    }
    if (allowPollingFallback) {
        portENTER_CRITICAL(&_inputMux);
        _softwarePulseCount = 0;
        _usingSoftwareCounter = true;
        portEXIT_CRITICAL(&_inputMux);
        ESP_LOGW(TAG, "GPIO%d using GPIO-interrupt pulse counter; PCNT setup failed: %s",
                 _pin, esp_err_to_name(err));
    } else {
        portENTER_CRITICAL(&_inputMux);
        _usingSoftwareCounter = false;
        portEXIT_CRITICAL(&_inputMux);
    }
    return err;
}

void WAZEH_Channel::initPulseCounter(bool countBothEdges, uint16_t filterUs)
{
    const WAZEH_CycleTime_Edge edge = countBothEdges
        ? WAZEH_CycleTime_Edge::Both
        : WAZEH_CycleTime_Edge::Rising;
    const esp_err_t err = initPulseCounterChecked(edge, static_cast<uint32_t>(filterUs) * 1000, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GPIO%d PCNT unavailable (%s); backend=%s",
                 _pin, esp_err_to_name(err),
                 _usingSoftwareCounter ? "GPIO interrupt" : "unavailable");
    }
}

void WAZEH_Channel::beginPulseCounter(pcnt_unit_handle_t unit, bool countBothEdges, uint16_t filterUs)
{
    if (unit != nullptr) {
        if (!_configured) {
            _lastCounterError = ESP_ERR_INVALID_STATE;
            return;
        }
#if SOC_PCNT_SUPPORTED
        esp_err_t err = releasePulseCounter();
        if (err == ESP_OK) {
            pcnt_chan_config_t channelConfig{};
            channelConfig.edge_gpio_num = static_cast<gpio_num_t>(_pin);
            channelConfig.level_gpio_num = -1;
            err = pcnt_new_channel(unit, &channelConfig, &_pcntChannel);
        }
        if (err == ESP_OK) {
            err = pcnt_channel_set_edge_action(
                _pcntChannel,
                PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                countBothEdges ? PCNT_CHANNEL_EDGE_ACTION_INCREASE
                               : PCNT_CHANNEL_EDGE_ACTION_HOLD);
        }
        if (err == ESP_OK) {
            err = pcnt_channel_set_level_action(
                _pcntChannel,
                PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                PCNT_CHANNEL_LEVEL_ACTION_KEEP);
        }
        if (err == ESP_OK && filterUs > 0) {
            pcnt_glitch_filter_config_t filterConfig{};
            filterConfig.max_glitch_ns = static_cast<uint32_t>(filterUs) * 1000;
            err = pcnt_unit_set_glitch_filter(unit, &filterConfig);
        }
        if (err == ESP_OK) {
            _pcntUnit = unit;
            _ownsPcntUnit = false;
            _pcntInitialized = true;
            portENTER_CRITICAL(&_inputMux);
            _usingSoftwareCounter = false;
            portEXIT_CRITICAL(&_inputMux);
            _lastCounterError = ESP_OK;
        } else {
            _pcntUnit = unit;
            _ownsPcntUnit = false;
            if (_pcntChannel != nullptr) {
                const esp_err_t cleanupErr = releasePulseCounter();
                if (cleanupErr != ESP_OK) {
                    ESP_LOGE(TAG, "GPIO%d external PCNT rollback failed: %s",
                             _pin, esp_err_to_name(cleanupErr));
                    err = cleanupErr;
                }
            }
            _lastCounterError = err;
            ESP_LOGE(TAG, "GPIO%d external PCNT channel setup failed: %s",
                     _pin, esp_err_to_name(err));
        }
#else
        _lastCounterError = ESP_ERR_NOT_SUPPORTED;
#endif
        return;
    }
    initPulseCounter(countBothEdges, filterUs);
}

esp_err_t WAZEH_Channel::useSoftwareCounterChecked()
{
    if (!_configured) {
        _lastCounterError = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t previousTotal = _hasLastKnownTotalCount ? _lastKnownTotalCount : _pulseCountOffset;
    if (isCounterAvailable()) {
        uint64_t liveTotal = 0;
        const esp_err_t readErr = getPulseCount(&liveTotal);
        if (readErr == ESP_OK) {
            previousTotal = liveTotal;
        } else if (_pcntInitialized) {
            // Recovery must not depend on the failing PCNT read path. Preserve the
            // last valid snapshot, switch backends, and report that the count may
            // have a discontinuity for pulses since the snapshot.
            ESP_LOGW(TAG,
                     "GPIO%d PCNT read failed (%s); switching with last known total=%" PRIu64
                     ". Counts since that snapshot may be lost.",
                     _pin, esp_err_to_name(readErr), previousTotal);
        } else {
            _lastCounterError = readErr;
            return readErr;
        }
    }

    const esp_err_t err = releasePulseCounter();
    if (err != ESP_OK) {
        _lastCounterError = err;
        return err;
    }

    _pulseCountOffset = previousTotal;
    _lastKnownTotalCount = previousTotal;
    _hasLastKnownTotalCount = true;
    portENTER_CRITICAL(&_inputMux);
    _softwarePulseCount = 0;
    _usingSoftwareCounter = true;
    portEXIT_CRITICAL(&_inputMux);
    _lastPulseSampleCount = previousTotal;
    _lastPulseSampleUs = esp_timer_get_time();
    _lastRateSampleCount = previousTotal;
    _lastRateSampleUs = _lastPulseSampleUs;
    _lastCounterError = ESP_OK;
    return ESP_OK;
}

void WAZEH_Channel::useSoftwareCounter()
{
    const esp_err_t err = useSoftwareCounterChecked();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d software-counter setup failed: %s", _pin, esp_err_to_name(err));
    }
}
