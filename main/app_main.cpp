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

// ESP32-S3 loopback wiring: GPIO4 output -> GPIO5 input.
constexpr gpio_num_t SQUARE_WAVE_GPIO = GPIO_NUM_4;
constexpr gpio_num_t PULSE_INPUT_GPIO = GPIO_NUM_5;
constexpr double OUTPUT_FREQUENCY_HZ = 15; 
constexpr uint32_t SAMPLE_INTERVAL_MS = 5000;
constexpr uint32_t PCNT_DIAGNOSTIC_TIMEOUT_MS = 3500;
constexpr uint32_t PCNT_GLITCH_FILTER_NS = 10000; // 10 us; within ESP32-S3 PCNT limits.

esp_timer_handle_t square_wave_timer = nullptr;
std::atomic<uint32_t> square_wave_toggle_count{0};
std::atomic<int> square_wave_output_level{0};

void toggleSquareWave(void *)
{
    const int next_level =
        square_wave_output_level.load(std::memory_order_relaxed) == 0 ? 1 : 0;
    const esp_err_t err = gpio_set_level(SQUARE_WAVE_GPIO, next_level);
    if (err != ESP_OK) {
        // Do not call ESP_ERROR_CHECK from a periodic callback: report the failure
        // and let the main task continue to diagnose the stopped/invalid signal.
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
        ESP_LOGE(TAG, "Output frequency must be a finite positive value");
        return ESP_ERR_INVALID_ARG;
    }

    const double half_period_us_exact = 500000.0 / OUTPUT_FREQUENCY_HZ;
    if (!std::isfinite(half_period_us_exact) ||
        half_period_us_exact < 1.0 ||
        half_period_us_exact >= 18446744073709551616.0) {
        ESP_LOGE(TAG, "Output frequency is outside the representable timer range");
        return ESP_ERR_INVALID_ARG;
    }
    const uint64_t half_period_us =
        static_cast<uint64_t>(half_period_us_exact + 0.5);

    gpio_config_t output_config = {};
    output_config.pin_bit_mask = 1ULL << SQUARE_WAVE_GPIO;
    output_config.mode = GPIO_MODE_OUTPUT;
    output_config.intr_type = GPIO_INTR_DISABLE;
    output_config.pull_up_en = GPIO_PULLUP_DISABLE;
    output_config.pull_down_en = GPIO_PULLDOWN_DISABLE;

    esp_err_t err = gpio_config(&output_config);
    if (err != ESP_OK) return err;

    err = gpio_set_level(SQUARE_WAVE_GPIO, 0);
    if (err != ESP_OK) return err;
    square_wave_output_level.store(0, std::memory_order_relaxed);

    esp_timer_create_args_t timer_args = {};
    timer_args.callback = toggleSquareWave;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = "square_wave";

    err = esp_timer_create(&timer_args, &square_wave_timer);
    if (err != ESP_OK) return err;

    err = esp_timer_start_periodic(square_wave_timer, half_period_us);
    if (err != ESP_OK) {
        (void)esp_timer_delete(square_wave_timer);
        square_wave_timer = nullptr;
        return err;
    }

    ESP_LOGI(TAG,
             "Square wave started: GPIO%d, target=%.9g"
             " Hz, duty=50%%, half-period=%llu us",
             SQUARE_WAVE_GPIO, OUTPUT_FREQUENCY_HZ,
             static_cast<unsigned long long>(half_period_us));
    return ESP_OK;
}

} // namespace

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "WAZEH_CycleTime ESP32-S3 pulse-counter diagnostic demo");
    ESP_LOGI(TAG, "Wire GPIO%d directly to GPIO%d; do not apply 5 V",
             SQUARE_WAVE_GPIO, PULSE_INPUT_GPIO);

    esp_err_t err = configureSquareWaveOutput();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Square-wave setup failed: %s", esp_err_to_name(err));
        stopSquareWaveOutput();
        return;
    }

    // Use typed configuration so input polarity and pull behavior are explicit.
    WAZEH_CycleTime cycle_time;
    WAZEH_CycleTime_ChannelConfig input_config{};
    input_config.pin = PULSE_INPUT_GPIO;
    input_config.input_mode = CYCLETIME_INPUT_MODE_SWITCH;
    input_config.active_level = WAZEH_CycleTime_ActiveLevel::High;
    input_config.pull = WAZEH_CycleTime_Pull::Down;
    input_config.debounce_ms = 0; // Pulse counter is edge based; no debounce is needed.

    err = cycle_time.addChannel(input_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Input GPIO%d setup failed: %s",
                 PULSE_INPUT_GPIO, esp_err_to_name(err));
        stopSquareWaveOutput();
        return;
    }

    // Prefer hardware PCNT. If setup fails, the component may have enabled its
    // GPIO-interrupt fallback; the checked API intentionally reports the PCNT error.
    const esp_err_t pcnt_init_err = cycle_time.initPulseCounterByPinChecked(
        PULSE_INPUT_GPIO,
        WAZEH_CycleTime_Edge::Rising,
        PCNT_GLITCH_FILTER_NS,
        true);

    WAZEH_Channel *channel = cycle_time.getChannel(PULSE_INPUT_GPIO);
    if (channel == nullptr || !channel->isCounterAvailable()) {
        ESP_LOGE(TAG, "No counter backend available. PCNT init result: %s",
                 esp_err_to_name(pcnt_init_err));
        (void)cycle_time.deinit();
        stopSquareWaveOutput();
        return;
    }

    if (pcnt_init_err != ESP_OK) {
        ESP_LOGW(TAG, "PCNT initialization returned %s; checking fallback backend",
                 esp_err_to_name(pcnt_init_err));
    }

    ESP_LOGI(TAG, "Initial backend: %s; last counter error: %s",
             channel->isPcntInitialized() ? "hardware PCNT" : "GPIO interrupt",
             esp_err_to_name(channel->getLastCounterError()));

    uint64_t initial_total = 0;
    err = cycle_time.getPulseCount(PULSE_INPUT_GPIO, &initial_total);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Initial counter read failed: %s", esp_err_to_name(err));
        (void)cycle_time.deinit();
        stopSquareWaveOutput();
        return;
    }

    int last_observed_input_level = gpio_get_level(PULSE_INPUT_GPIO);
    uint64_t diagnostic_rising_edges = 0;
    const int64_t start_us = esp_timer_get_time();
    int64_t last_sample_us = start_us;
    int64_t rate_start_us = start_us;
    uint64_t previous_sample_total = initial_total;
    uint64_t rate_start_total = initial_total;
    uint64_t last_seen_total = initial_total;
    uint32_t previous_toggle_count =
        square_wave_toggle_count.load(std::memory_order_relaxed);
    uint32_t sample_number = 0;
    bool hardware_pcnt_stalled = false;
    bool pcnt_read_fallback_attempted = false;
    bool interrupt_counter_stalled_reported = false;
    int64_t interrupt_watchdog_start_us = start_us;
    uint64_t interrupt_watchdog_start_rising = 0;
    uint64_t interrupt_watchdog_start_total = initial_total;
    esp_err_t last_reported_count_error = ESP_OK;

    while (true) {
        // Required for component debounce/events; pulse edges themselves are captured
        // by PCNT or the GPIO ISR and do not depend on this loop's polling rate.
        (void)cycle_time.loop();

        const int input_level = gpio_get_level(PULSE_INPUT_GPIO);
        if (input_level >= 0 && last_observed_input_level >= 0) {
            if (last_observed_input_level == 0 && input_level == 1) {
                ++diagnostic_rising_edges;
            }
            last_observed_input_level = input_level;
        } else if (input_level >= 0) {
            last_observed_input_level = input_level;
        }

        uint64_t total_count = 0;
        const esp_err_t count_err = cycle_time.getPulseCount(PULSE_INPUT_GPIO, &total_count);
        const int64_t now_us = esp_timer_get_time();

        if (count_err != ESP_OK) {
            if (count_err != last_reported_count_error) {
                ESP_LOGE(TAG, "Pulse-counter read failed: %s; backend=%d",
                         esp_err_to_name(count_err),
                         static_cast<int>(channel->getCounterBackend()));
                last_reported_count_error = count_err;
            }
            if (channel->isPcntInitialized() && !pcnt_read_fallback_attempted) {
                pcnt_read_fallback_attempted = true;
                const esp_err_t fallback_err = channel->useSoftwareCounterChecked();
                hardware_pcnt_stalled = true;
                if (fallback_err == ESP_OK) {
                    ESP_LOGW(TAG, "Switched to GPIO-interrupt fallback after PCNT read error");
                    if (cycle_time.getPulseCount(PULSE_INPUT_GPIO, &total_count) == ESP_OK) {
                        previous_sample_total = total_count;
                        rate_start_total = total_count;
                        rate_start_us = now_us;
                        last_seen_total = total_count;
                        last_sample_us = now_us;
                        interrupt_watchdog_start_us = now_us;
                        interrupt_watchdog_start_rising = diagnostic_rising_edges;
                        interrupt_watchdog_start_total = total_count;
                        interrupt_counter_stalled_reported = false;
                    }
                } else {
                    ESP_LOGE(TAG, "Could not switch to software counter: %s",
                             esp_err_to_name(fallback_err));
                }
            }
        } else {
            last_reported_count_error = ESP_OK;
            last_seen_total = total_count;
        }

        // If GPIO5 demonstrably sees rising edges but PCNT remains at zero, fall back
        // to the ISR counter instead of silently reporting 0 Hz forever.
        const uint64_t elapsed_since_start_ms =
            static_cast<uint64_t>(now_us - start_us) / 1000ULL;
        if (!hardware_pcnt_stalled && channel->isPcntInitialized() &&
            elapsed_since_start_ms >= PCNT_DIAGNOSTIC_TIMEOUT_MS &&
            diagnostic_rising_edges >= 2 && last_seen_total == initial_total) {
            ESP_LOGE(TAG,
                     "PCNT diagnostic failed: GPIO5 observed %" PRIu64
                     " rising edges but PCNT total did not advance. Switching backend.",
                     diagnostic_rising_edges);
            const esp_err_t fallback_err = channel->useSoftwareCounterChecked();
            hardware_pcnt_stalled = true;
            if (fallback_err == ESP_OK) {
                (void)cycle_time.getPulseCount(PULSE_INPUT_GPIO, &total_count);
                previous_sample_total = total_count;
                rate_start_total = total_count;
                rate_start_us = now_us;
                last_seen_total = total_count;
                last_sample_us = now_us;
                interrupt_watchdog_start_us = now_us;
                interrupt_watchdog_start_rising = diagnostic_rising_edges;
                interrupt_watchdog_start_total = total_count;
                interrupt_counter_stalled_reported = false;
                ESP_LOGW(TAG, "Active backend is now GPIO interrupt fallback");
            } else {
                ESP_LOGE(TAG, "Fallback switch failed: %s", esp_err_to_name(fallback_err));
            }
        }

        if (!channel->isPcntInitialized() &&
            channel->getCounterBackend() == WAZEH_CycleTime_CounterBackend::Interrupt &&
            !interrupt_counter_stalled_reported &&
            now_us - interrupt_watchdog_start_us >=
                static_cast<int64_t>(PCNT_DIAGNOSTIC_TIMEOUT_MS) * 1000 &&
            diagnostic_rising_edges >= interrupt_watchdog_start_rising + 2 &&
            last_seen_total == interrupt_watchdog_start_total) {
            ESP_LOGE(TAG,
                     "GPIO-interrupt counter diagnostic failed: input monitor observed "
                     "new rising edges but the component count did not advance. "
                     "Check GPIO ISR registration and component state.");
            interrupt_counter_stalled_reported = true;
        }

        if (now_us - last_sample_us >=
            static_cast<int64_t>(SAMPLE_INTERVAL_MS) * 1000) {
            // Use a cumulative count and actual elapsed time. Never clear the counter
            // at sample boundaries, avoiding the read/reset race and lost edge window.
            uint64_t sample_total = 0;
            const esp_err_t sample_err =
                cycle_time.getPulseCount(PULSE_INPUT_GPIO, &sample_total);
            if (sample_err == ESP_OK && sample_total >= previous_sample_total) {
                const uint64_t pulses = sample_total - previous_sample_total;
                const int64_t elapsed_us = now_us - last_sample_us;
                const double measured_hz = elapsed_us > 0
                    ? static_cast<double>(pulses) * 1000000.0 /
                      static_cast<double>(elapsed_us)
                    : 0.0;
                const int64_t average_elapsed_us = now_us - rate_start_us;
                const double average_hz =
                    average_elapsed_us > 0 && sample_total >= rate_start_total
                    ? static_cast<double>(sample_total - rate_start_total) *
                      1000000.0 / static_cast<double>(average_elapsed_us)
                    : 0.0;

                ++sample_number;
                const uint32_t toggle_count =
                    square_wave_toggle_count.load(std::memory_order_relaxed);
                const int output_level = gpio_get_level(SQUARE_WAVE_GPIO);
                const int final_input_level = gpio_get_level(PULSE_INPUT_GPIO);

                ESP_LOGI(TAG,
                         "Sample %" PRIu32
                         " | target=%.9g Hz"
                         " | backend=%s"
                         " | interval pulses=%" PRIu64
                         " | total pulses=%" PRIu64
                         " | interval rate=%0.3f Hz"
                         " | running average=%0.3f Hz"
                         " | diagnostic rising=%" PRIu64
                         " | toggles=%" PRIu32
                         " | GPIO%d=%d GPIO%d=%d",
                         sample_number, OUTPUT_FREQUENCY_HZ,
                         channel->isPcntInitialized() ? "PCNT" : "GPIO-ISR",
                         pulses, sample_total, measured_hz, average_hz,
                         diagnostic_rising_edges,
                         toggle_count - previous_toggle_count,
                         SQUARE_WAVE_GPIO, output_level,
                         PULSE_INPUT_GPIO, final_input_level);

                previous_sample_total = sample_total;
                previous_toggle_count = toggle_count;
                last_sample_us = now_us;
            } else if (sample_err != ESP_OK) {
                ESP_LOGE(TAG, "Sample read failed: %s", esp_err_to_name(sample_err));
                // Avoid flooding the console if the backend remains unavailable.
                last_sample_us = now_us;
            } else {
                ESP_LOGE(TAG, "Cumulative pulse count unexpectedly decreased");
                previous_sample_total = sample_total;
                last_sample_us = now_us;
            }
        }

        // Delay by at least one RTOS tick. Sub-tick millisecond delays round down
        // to zero (for example, 5 ms with the project's 100 Hz tick rate).
        vTaskDelay(1);
    }
}
