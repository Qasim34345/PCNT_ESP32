#include <inttypes.h>
#include <atomic>
#include "WAZEH_CycleTime.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr char TAG[] = "DualInputExample";
constexpr gpio_num_t OUTPUT_GPIO = GPIO_NUM_4;
constexpr gpio_num_t INPUT_A = GPIO_NUM_5;
constexpr gpio_num_t INPUT_B = GPIO_NUM_6;
constexpr uint64_t HALF_PERIOD_US = 500000; // 1 Hz square wave, 50% duty
esp_timer_handle_t timer_handle = nullptr;
std::atomic<int> output_level{0};

void toggleOutput(void *)
{
    const int next = output_level.load(std::memory_order_relaxed) ? 0 : 1;
    if (gpio_set_level(OUTPUT_GPIO, next) == ESP_OK) {
        output_level.store(next, std::memory_order_relaxed);
    }
}

esp_err_t addPulseInput(WAZEH_CycleTime &component, gpio_num_t pin)
{
    WAZEH_CycleTime_ChannelConfig config{};
    config.pin = pin;
    config.input_mode = CYCLETIME_INPUT_MODE_SWITCH;
    config.active_level = WAZEH_CycleTime_ActiveLevel::High;
    config.pull = WAZEH_CycleTime_Pull::Down;
    config.debounce_ms = 0;
    esp_err_t err = component.addChannel(config);
    if (err != ESP_OK) return err;
    err = component.initPulseCounterByPinChecked(pin, WAZEH_CycleTime_Edge::Rising, 0, true);
    WAZEH_Channel *channel = component.getChannel(pin);
    if (err != ESP_OK && (channel == nullptr || !channel->isCounterAvailable())) return err;
    ESP_LOGI(TAG, "GPIO%d backend=%s", pin,
             channel != nullptr && channel->isPcntInitialized() ? "PCNT" : "GPIO interrupt");
    return ESP_OK;
}
}

extern "C" void app_main(void)
{
    gpio_config_t output_config{};
    output_config.pin_bit_mask = 1ULL << OUTPUT_GPIO;
    output_config.mode = GPIO_MODE_OUTPUT;
    output_config.intr_type = GPIO_INTR_DISABLE;
    output_config.pull_up_en = GPIO_PULLUP_DISABLE;
    output_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    esp_err_t err = gpio_config(&output_config);
    if (err != ESP_OK || gpio_set_level(OUTPUT_GPIO, 0) != ESP_OK) {
        ESP_LOGE(TAG, "Could not configure output GPIO%d", OUTPUT_GPIO);
        return;
    }

    WAZEH_CycleTime cycleTime;
    err = addPulseInput(cycleTime, INPUT_A);
    if (err == ESP_OK) err = addPulseInput(cycleTime, INPUT_B);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Input setup failed: %s", esp_err_to_name(err));
        (void)cycleTime.deinit();
        (void)gpio_reset_pin(OUTPUT_GPIO);
        return;
    }

    esp_timer_create_args_t timer_args{};
    timer_args.callback = toggleOutput;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = "loopback_1hz";
    err = esp_timer_create(&timer_args, &timer_handle);
    if (err == ESP_OK) err = esp_timer_start_periodic(timer_handle, HALF_PERIOD_US);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not start waveform timer: %s", esp_err_to_name(err));
        if (timer_handle != nullptr) (void)esp_timer_delete(timer_handle);
        (void)cycleTime.deinit();
        (void)gpio_reset_pin(OUTPUT_GPIO);
        return;
    }

    ESP_LOGI(TAG, "Wire GPIO%d output to GPIO%d and GPIO%d inputs; common ground.",
             OUTPUT_GPIO, INPUT_A, INPUT_B);
    ESP_LOGW(TAG, "Software-timer waveform is for low-speed functional tests, not precision testing.");
    uint64_t previous_a = 0, previous_b = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint64_t count_a = 0, count_b = 0;
        const esp_err_t err_a = cycleTime.getPulseCount(INPUT_A, &count_a);
        const esp_err_t err_b = cycleTime.getPulseCount(INPUT_B, &count_b);
        if (err_a == ESP_OK && err_b == ESP_OK) {
            ESP_LOGI(TAG, "GPIO%d total=%" PRIu64 " (+%" PRIu64 "), GPIO%d total=%" PRIu64 " (+%" PRIu64 ")",
                     INPUT_A, count_a, count_a - previous_a, INPUT_B, count_b, count_b - previous_b);
            previous_a = count_a;
            previous_b = count_b;
        } else {
            ESP_LOGE(TAG, "Counter read error: GPIO%d=%s GPIO%d=%s", INPUT_A,
                     esp_err_to_name(err_a), INPUT_B, esp_err_to_name(err_b));
        }
    }
}
