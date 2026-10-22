#include <inttypes.h>
#include <stdint.h>
#include <atomic>
#include <cmath>

#include "WAZEH_CycleTime.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr char TAG[] = "CycleTimeDemo";

// One output drives every configured input in parallel.
constexpr gpio_num_t SQUARE_WAVE_GPIO = GPIO_NUM_4;

constexpr double OUTPUT_FREQUENCY_HZ = 5.555;
constexpr uint32_t REPORT_INTERVAL_MS = 1000;
constexpr double PULSE_INACTIVITY_TIMEOUT_PERIODS = 1.5;
constexpr uint32_t MIN_PULSE_INACTIVITY_TIMEOUT_MS = 100;
constexpr double COUNTER_WATCHDOG_PERIODS = 2.5;
constexpr uint32_t MIN_COUNTER_WATCHDOG_MS = 500;
constexpr uint32_t RECOVERY_RETRY_MS = 2000;
constexpr uint32_t PCNT_GLITCH_FILTER_NS = 10000;

esp_timer_handle_t square_wave_timer = nullptr;
std::atomic<uint32_t> square_wave_toggle_count{0};
std::atomic<int> square_wave_output_level{0};

struct InputMonitor {
    gpio_num_t pin;
    WAZEH_Channel *channel = nullptr;
    int last_observed_level = -1;
    uint64_t diagnostic_rising_edges = 0;
    uint64_t total_count = 0;
    uint64_t rate_observed_total = 0;
    uint64_t active_pulse_count = 0;
    uint64_t watchdog_start_total = 0;
    uint64_t watchdog_start_rising_edges = 0;
    int64_t last_pulse_us = 0;
    int64_t watchdog_start_us = 0;
    int64_t last_recovery_attempt_us = 0;
    double live_rate_hz = 0.0;
    double live_cycle_time_ms = 0.0;
    bool fallback_attempted = false;
    bool interrupt_stall_reported = false;
    esp_err_t last_read_error = ESP_OK;
};

// Select the input pins here. Each entry is configured and counted independently.
InputMonitor monitors[] = {
    {GPIO_NUM_5},
    {GPIO_NUM_6}
};
constexpr size_t INPUT_COUNT = sizeof(monitors) / sizeof(monitors[0]);
static_assert(INPUT_COUNT > 0, "Configure at least one pulse input");

const char *gpioLevelName(int level)
{
    if (level == 0) return "LOW";
    if (level == 1) return "HIGH";
    return "ERR";
}

bool validateInputPins()
{
    if (INPUT_COUNT > WAZEH_CYCLETIME_MAX_CHANNELS) {
        ESP_LOGE(TAG, "%u inputs exceed component channel capacity (%u)",
                 static_cast<unsigned>(INPUT_COUNT),
                 static_cast<unsigned>(WAZEH_CYCLETIME_MAX_CHANNELS));
        return false;
    }

    for (size_t i = 0; i < INPUT_COUNT; ++i) {
        if (!GPIO_IS_VALID_GPIO(monitors[i].pin) ||
            monitors[i].pin == SQUARE_WAVE_GPIO) {
            ESP_LOGE(TAG, "Invalid or conflicting input pin GPIO%d", monitors[i].pin);
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (monitors[i].pin == monitors[j].pin) {
                ESP_LOGE(TAG, "Duplicate input pin GPIO%d", monitors[i].pin);
                return false;
            }
        }
    }
    return true;
}

void toggleSquareWave(void *)
{
    const int next_level =
        square_wave_output_level.load(std::memory_order_relaxed) == 0 ? 1 : 0;
    const esp_err_t err = gpio_set_level(SQUARE_WAVE_GPIO, next_level);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_set_level(GPIO%d) failed: %s",
                 SQUARE_WAVE_GPIO, esp_err_to_name(err));
        return;
    }
    square_wave_output_level.store(next_level, std::memory_order_relaxed);
    square_wave_toggle_count.fetch_add(1, std::memory_order_relaxed);
}

void stopSquareWaveOutput()
{
    if (square_wave_timer != nullptr) {
        if (esp_timer_is_active(square_wave_timer)) {
            (void)esp_timer_stop(square_wave_timer);
        }
        (void)esp_timer_delete(square_wave_timer);
        square_wave_timer = nullptr;
    }
    (void)gpio_set_level(SQUARE_WAVE_GPIO, 0);
    square_wave_output_level.store(0, std::memory_order_relaxed);
}

esp_err_t configureSquareWaveOutput()
{
    if (!std::isfinite(OUTPUT_FREQUENCY_HZ) || OUTPUT_FREQUENCY_HZ <= 0.0) {
        return ESP_ERR_INVALID_ARG;
    }

    const double half_period_us_exact = 500000.0 / OUTPUT_FREQUENCY_HZ;
    if (!std::isfinite(half_period_us_exact) || half_period_us_exact < 1.0 ||
        half_period_us_exact >= 18446744073709551616.0) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint64_t half_period_us = static_cast<uint64_t>(half_period_us_exact + 0.5);

    gpio_config_t output_config{};
    output_config.pin_bit_mask = 1ULL << SQUARE_WAVE_GPIO;
    output_config.mode = GPIO_MODE_OUTPUT;
    output_config.intr_type = GPIO_INTR_DISABLE;
    output_config.pull_up_en = GPIO_PULLUP_DISABLE;
    output_config.pull_down_en = GPIO_PULLDOWN_DISABLE;

    esp_err_t err = gpio_config(&output_config);
    if (err != ESP_OK) return err;

    err = gpio_set_level(SQUARE_WAVE_GPIO, 0);
    if (err != ESP_OK) {
        (void)gpio_reset_pin(SQUARE_WAVE_GPIO);
        return err;
    }
    square_wave_output_level.store(0, std::memory_order_relaxed);

    esp_timer_create_args_t timer_args{};
    timer_args.callback = toggleSquareWave;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = "square_wave";

    err = esp_timer_create(&timer_args, &square_wave_timer);
    if (err != ESP_OK) {
        (void)gpio_reset_pin(SQUARE_WAVE_GPIO);
        return err;
    }

    err = esp_timer_start_periodic(square_wave_timer, half_period_us);
    if (err != ESP_OK) {
        (void)esp_timer_delete(square_wave_timer);
        square_wave_timer = nullptr;
        (void)gpio_reset_pin(SQUARE_WAVE_GPIO);
        return err;
    }

    ESP_LOGI(TAG, "Square wave: GPIO%d, target=%.3f Hz, duty=50%%, half-period=%" PRIu64 " us",
             SQUARE_WAVE_GPIO, OUTPUT_FREQUENCY_HZ, half_period_us);
    return ESP_OK;
}

void rebaseMonitor(InputMonitor &monitor, uint64_t total, int64_t now_us)
{
    monitor.total_count = total;
    monitor.rate_observed_total = total;
    monitor.active_pulse_count = 0;
    monitor.last_pulse_us = 0;
    monitor.live_rate_hz = 0.0;
    monitor.live_cycle_time_ms = 0.0;
    monitor.watchdog_start_us = now_us;
    monitor.watchdog_start_rising_edges = monitor.diagnostic_rising_edges;
    monitor.watchdog_start_total = total;
    monitor.interrupt_stall_reported = false;
}

bool initializeInput(WAZEH_CycleTime &cycle_time, InputMonitor &monitor, int64_t now_us)
{
    WAZEH_CycleTime_ChannelConfig config{};
    config.pin = monitor.pin;
    config.input_mode = CYCLETIME_INPUT_MODE_SWITCH;
    config.active_level = WAZEH_CycleTime_ActiveLevel::High;
    config.pull = WAZEH_CycleTime_Pull::Down;
    config.debounce_ms = 0;

    esp_err_t err = cycle_time.addChannel(config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d channel setup failed: %s",
                 monitor.pin, esp_err_to_name(err));
        return false;
    }

    const esp_err_t pcnt_err = cycle_time.initPulseCounterByPinChecked(
        monitor.pin, WAZEH_CycleTime_Edge::Rising, PCNT_GLITCH_FILTER_NS, true);
    monitor.channel = cycle_time.getChannel(static_cast<uint8_t>(monitor.pin));
    if (monitor.channel == nullptr || !monitor.channel->isCounterAvailable()) {
        ESP_LOGE(TAG, "GPIO%d has no counter backend; PCNT init: %s",
                 monitor.pin, esp_err_to_name(pcnt_err));
        return false;
    }
    if (pcnt_err != ESP_OK) {
        ESP_LOGW(TAG, "GPIO%d PCNT setup returned %s; GPIO-interrupt fallback is active",
                 monitor.pin, esp_err_to_name(pcnt_err));
    }

    uint64_t initial_total = 0;
    err = cycle_time.getPulseCount(static_cast<uint8_t>(monitor.pin), &initial_total);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d initial count read failed: %s",
                 monitor.pin, esp_err_to_name(err));
        return false;
    }

    monitor.last_observed_level = gpio_get_level(monitor.pin);
    monitor.diagnostic_rising_edges = 0;
    monitor.last_recovery_attempt_us = now_us;
    monitor.last_read_error = ESP_OK;
    rebaseMonitor(monitor, initial_total, now_us);
    ESP_LOGI(TAG, "GPIO%d ready: backend=%s, total=%" PRIu64,
             monitor.pin,
             monitor.channel->isPcntInitialized() ? "hardware PCNT" : "GPIO interrupt",
             initial_total);
    return true;
}

void monitorInput(WAZEH_CycleTime &cycle_time, InputMonitor &monitor,
                 int64_t now_us, int64_t pulse_timeout_us,
                 int64_t counter_watchdog_us)
{
    const int level = gpio_get_level(monitor.pin);
    if (level >= 0 && monitor.last_observed_level >= 0) {
        if (monitor.last_observed_level == 0 && level == 1 &&
            monitor.diagnostic_rising_edges != UINT64_MAX) {
            ++monitor.diagnostic_rising_edges;
        }
        monitor.last_observed_level = level;
    } else if (level >= 0) {
        monitor.last_observed_level = level;
    }

    uint64_t total = 0;
    const esp_err_t count_err =
        cycle_time.getPulseCount(static_cast<uint8_t>(monitor.pin), &total);
    if (count_err == ESP_OK) {
        monitor.last_read_error = ESP_OK;
        monitor.total_count = total;
        if (total > monitor.rate_observed_total) {
            const uint64_t new_pulses = total - monitor.rate_observed_total;
            const int64_t pulse_interval_us = now_us - monitor.last_pulse_us;
            if (monitor.last_pulse_us > 0 && pulse_interval_us > 0 &&
                pulse_interval_us <= pulse_timeout_us) {
                monitor.live_rate_hz =
                    static_cast<double>(new_pulses) * 1000000.0 /
                    static_cast<double>(pulse_interval_us);
                monitor.live_cycle_time_ms =
                    static_cast<double>(pulse_interval_us) /
                    (1000.0 * static_cast<double>(new_pulses));
            } else {
                monitor.active_pulse_count = 0;
                monitor.live_rate_hz = 0.0;
                monitor.live_cycle_time_ms = 0.0;
            }
            monitor.active_pulse_count =
                UINT64_MAX - monitor.active_pulse_count < new_pulses
                ? UINT64_MAX
                : monitor.active_pulse_count + new_pulses;
            monitor.last_pulse_us = now_us;
            monitor.rate_observed_total = total;
        } else if (total < monitor.rate_observed_total) {
            rebaseMonitor(monitor, total, now_us);
        }
    } else {
        if (count_err != monitor.last_read_error) {
            ESP_LOGE(TAG, "GPIO%d counter read failed: %s; backend=%d",
                     monitor.pin, esp_err_to_name(count_err),
                     static_cast<int>(monitor.channel->getCounterBackend()));
            monitor.last_read_error = count_err;
        }
        if (monitor.channel->isPcntInitialized() && !monitor.fallback_attempted) {
            monitor.fallback_attempted = true;
            const esp_err_t fallback_err = monitor.channel->useSoftwareCounterChecked();
            if (fallback_err == ESP_OK) {
                ESP_LOGW(TAG, "GPIO%d switched to GPIO-interrupt fallback after PCNT read error",
                         monitor.pin);
                uint64_t recovered_total = 0;
                if (cycle_time.getPulseCount(static_cast<uint8_t>(monitor.pin),
                                             &recovered_total) == ESP_OK) {
                    rebaseMonitor(monitor, recovered_total, now_us);
                }
            } else {
                ESP_LOGE(TAG, "GPIO%d fallback switch failed: %s",
                         monitor.pin, esp_err_to_name(fallback_err));
            }
        }
    }

    if (monitor.channel->getCounterBackend() ==
            WAZEH_CycleTime_CounterBackend::Unavailable &&
        now_us - monitor.last_recovery_attempt_us >=
            static_cast<int64_t>(RECOVERY_RETRY_MS) * 1000) {
        monitor.last_recovery_attempt_us = now_us;
        const esp_err_t recovery_err = monitor.channel->useSoftwareCounterChecked();
        if (recovery_err == ESP_OK) {
            uint64_t recovered_total = 0;
            if (cycle_time.getPulseCount(static_cast<uint8_t>(monitor.pin),
                                         &recovered_total) == ESP_OK) {
                rebaseMonitor(monitor, recovered_total, now_us);
                ESP_LOGW(TAG, "GPIO%d recovered using GPIO-interrupt fallback", monitor.pin);
            }
        } else {
            ESP_LOGE(TAG, "GPIO%d backend recovery failed: %s",
                     monitor.pin, esp_err_to_name(recovery_err));
        }
    }

    if (now_us - monitor.watchdog_start_us >= counter_watchdog_us) {
        const bool input_progress =
            monitor.diagnostic_rising_edges >= monitor.watchdog_start_rising_edges &&
            monitor.diagnostic_rising_edges - monitor.watchdog_start_rising_edges >= 2;
        const bool counter_progress = monitor.total_count > monitor.watchdog_start_total;

        if (input_progress && !counter_progress) {
            if (monitor.channel->isPcntInitialized()) {
                ESP_LOGE(TAG,
                         "GPIO%d PCNT watchdog saw input edges but no count advance; trying GPIO-ISR fallback",
                         monitor.pin);
                const esp_err_t fallback_err = monitor.channel->useSoftwareCounterChecked();
                if (fallback_err == ESP_OK) {
                    monitor.fallback_attempted = true;
                    uint64_t recovered_total = 0;
                    if (cycle_time.getPulseCount(static_cast<uint8_t>(monitor.pin),
                                                 &recovered_total) == ESP_OK) {
                        rebaseMonitor(monitor, recovered_total, now_us);
                        ESP_LOGW(TAG, "GPIO%d switched to interrupt fallback; transition count may be uncertain",
                                 monitor.pin);
                    }
                } else {
                    ESP_LOGE(TAG, "GPIO%d fallback switch failed: %s",
                             monitor.pin, esp_err_to_name(fallback_err));
                }
            } else if (monitor.channel->getCounterBackend() ==
                           WAZEH_CycleTime_CounterBackend::Interrupt &&
                       !monitor.interrupt_stall_reported) {
                ESP_LOGE(TAG,
                         "GPIO%d interrupt counter did not advance despite observed input edges",
                         monitor.pin);
                monitor.interrupt_stall_reported = true;
            }
        } else if (counter_progress) {
            monitor.interrupt_stall_reported = false;
        }

        monitor.watchdog_start_us = now_us;
        monitor.watchdog_start_rising_edges = monitor.diagnostic_rising_edges;
        monitor.watchdog_start_total = monitor.total_count;
    }

    if (monitor.last_pulse_us == 0 ||
        now_us - monitor.last_pulse_us > pulse_timeout_us) {
        monitor.live_rate_hz = 0.0;
        monitor.live_cycle_time_ms = 0.0;
        monitor.active_pulse_count = 0;
    }
}

} // namespace

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "WAZEH_CycleTime ESP32-S3 multi-input pulse-counter demo");
    ESP_LOGI(TAG, "Signal: GPIO%d at %.3f Hz -> %u independent input(s)",
             SQUARE_WAVE_GPIO, OUTPUT_FREQUENCY_HZ,
             static_cast<unsigned>(INPUT_COUNT));
    for (size_t i = 0; i < INPUT_COUNT; ++i) {
        ESP_LOGI(TAG, "Configured pulse input: GPIO%d", monitors[i].pin);
    }
    ESP_LOGW(TAG, "Verify selected GPIOs are free on your board and never apply 5 V");

    if (!validateInputPins()) {
        return;
    }

    esp_err_t err = configureSquareWaveOutput();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Square-wave setup failed: %s", esp_err_to_name(err));
        stopSquareWaveOutput();
        return;
    }

    WAZEH_CycleTime cycle_time;
    const int64_t init_time_us = esp_timer_get_time();
    for (size_t i = 0; i < INPUT_COUNT; ++i) {
        if (!initializeInput(cycle_time, monitors[i], init_time_us)) {
            ESP_LOGE(TAG, "Initialization failed for GPIO%d; stopping demo",
                     monitors[i].pin);
            (void)cycle_time.deinit();
            stopSquareWaveOutput();
            return;
        }
    }

    int64_t pulse_timeout_us = static_cast<int64_t>(
        PULSE_INACTIVITY_TIMEOUT_PERIODS * 1000000.0 / OUTPUT_FREQUENCY_HZ);
    const int64_t minimum_timeout_us =
        static_cast<int64_t>(MIN_PULSE_INACTIVITY_TIMEOUT_MS) * 1000;
    if (pulse_timeout_us < minimum_timeout_us) {
        pulse_timeout_us = minimum_timeout_us;
    }
    int64_t counter_watchdog_us = static_cast<int64_t>(
        COUNTER_WATCHDOG_PERIODS * 1000000.0 / OUTPUT_FREQUENCY_HZ);
    const int64_t minimum_watchdog_us =
        static_cast<int64_t>(MIN_COUNTER_WATCHDOG_MS) * 1000;
    if (counter_watchdog_us < minimum_watchdog_us) {
        counter_watchdog_us = minimum_watchdog_us;
    }

    ESP_LOGI(TAG, "Each input counter is independent and reports its own rate and cycle time.");
    int64_t last_report_us = init_time_us;
    uint32_t sample_number = 0;
    uint32_t previous_toggle_count =
        square_wave_toggle_count.load(std::memory_order_relaxed);

    while (true) {
        (void)cycle_time.loop();
        const int64_t now_us = esp_timer_get_time();
        for (size_t i = 0; i < INPUT_COUNT; ++i) {
            monitorInput(cycle_time, monitors[i], now_us,
                         pulse_timeout_us, counter_watchdog_us);
        }

        if (now_us - last_report_us >=
            static_cast<int64_t>(REPORT_INTERVAL_MS) * 1000) {
            ++sample_number;
            const uint32_t toggle_count =
                square_wave_toggle_count.load(std::memory_order_relaxed);
            const int output_level = gpio_get_level(SQUARE_WAVE_GPIO);
            ESP_LOGI(TAG,
                     "Sample %" PRIu32 " | OUTPUT GPIO%d=%s(%d)"
                     " | target=%.3f Hz | toggles=%" PRIu32,
                     sample_number, SQUARE_WAVE_GPIO,
                     gpioLevelName(output_level), output_level,
                     OUTPUT_FREQUENCY_HZ, toggle_count - previous_toggle_count);

            for (size_t i = 0; i < INPUT_COUNT; ++i) {
                const InputMonitor &monitor = monitors[i];
                const int input_level = gpio_get_level(monitor.pin);
                const bool pulse_active =
                    monitor.last_pulse_us != 0 &&
                    now_us - monitor.last_pulse_us <= pulse_timeout_us;
                ESP_LOGI(TAG,
                         "Sample %" PRIu32 " | INPUT GPIO%d=%s(%d)"
                         " | backend=%s | counter_read=%s"
                         " | live rate=%.3f Hz"
                         " | cycle time=%.3f ms"
                         " | live pulses=%" PRIu64
                         " | total pulses=%" PRIu64
                         " | pulse=%s"
                         " | diagnostic_rising=%" PRIu64,
                         sample_number, monitor.pin,
                         gpioLevelName(input_level), input_level,
                         monitor.channel->isPcntInitialized() ? "PCNT" : "GPIO-ISR",
                         monitor.last_read_error == ESP_OK ? "OK" : "ERROR",
                         monitor.live_rate_hz, monitor.live_cycle_time_ms,
                         monitor.active_pulse_count, monitor.total_count,
                         pulse_active ? "ACTIVE" : "STALE",
                         monitor.diagnostic_rising_edges);
            }
            previous_toggle_count = toggle_count;
            last_report_us = now_us;
        }

        vTaskDelay(1);
    }
}
