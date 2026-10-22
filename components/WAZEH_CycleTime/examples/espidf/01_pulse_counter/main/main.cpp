#include <inttypes.h>
#include "WAZEH_CycleTime.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr char TAG[] = "PulseCounterExample";
constexpr gpio_num_t INPUT_GPIO = GPIO_NUM_5;
constexpr uint32_t SAMPLE_INTERVAL_MS = 1000;
constexpr uint32_t GLITCH_FILTER_NS = 0;
}

extern "C" void app_main(void)
{
    WAZEH_CycleTime cycleTime;
    WAZEH_CycleTime_ChannelConfig config{};
    config.pin = INPUT_GPIO;
    config.input_mode = CYCLETIME_INPUT_MODE_SWITCH;
    config.active_level = WAZEH_CycleTime_ActiveLevel::High;
    config.pull = WAZEH_CycleTime_Pull::Down;
    config.debounce_ms = 0;

    esp_err_t err = cycleTime.addChannel(config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "addChannel failed: %s", esp_err_to_name(err));
        return;
    }

    err = cycleTime.initPulseCounterByPinChecked(
        INPUT_GPIO, WAZEH_CycleTime_Edge::Rising, GLITCH_FILTER_NS, true);
    WAZEH_Channel *channel = cycleTime.getChannel(INPUT_GPIO);
    if (err != ESP_OK && (channel == nullptr || !channel->isCounterAvailable())) {
        ESP_LOGE(TAG, "Counter initialization failed: %s", esp_err_to_name(err));
        (void)cycleTime.deinit();
        return;
    }

    ESP_LOGI(TAG, "Pulse input GPIO%d; backend=%s",
             INPUT_GPIO,
             channel != nullptr && channel->isPcntInitialized() ? "PCNT" : "GPIO interrupt");
    ESP_LOGI(TAG, "Connect a 3.3 V pulse source and common ground. Sampling every %u ms.",
             static_cast<unsigned>(SAMPLE_INTERVAL_MS));

    uint64_t previous = 0;
    int64_t previous_us = esp_timer_get_time();
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_INTERVAL_MS));
        const int64_t now_us = esp_timer_get_time();
        uint64_t total = 0;
        err = cycleTime.getPulseCount(INPUT_GPIO, &total);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "getPulseCount failed: %s", esp_err_to_name(err));
            continue;
        }
        const uint64_t delta = total - previous;
        const double elapsed_s = static_cast<double>(now_us - previous_us) / 1000000.0;
        const double rate_hz = elapsed_s > 0.0 ? static_cast<double>(delta) / elapsed_s : 0.0;
        ESP_LOGI(TAG, "GPIO%d total=%" PRIu64 ", interval=%" PRIu64 ", estimated_rate=%.3f Hz",
                 INPUT_GPIO, total, delta, rate_hz);
        previous = total;
        previous_us = now_us;
    }
}
