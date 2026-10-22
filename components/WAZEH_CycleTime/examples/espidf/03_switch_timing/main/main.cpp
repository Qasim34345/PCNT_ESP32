#include "WAZEH_CycleTime.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr char TAG[] = "SwitchTimingExample";
constexpr gpio_num_t SWITCH_GPIO = GPIO_NUM_5;
}

extern "C" void app_main(void)
{
    WAZEH_CycleTime cycleTime;
    WAZEH_CycleTime_ChannelConfig config{};
    config.pin = SWITCH_GPIO;
    config.input_mode = CYCLETIME_INPUT_MODE_PUSHBUTTON;
    config.active_level = WAZEH_CycleTime_ActiveLevel::Low;
    config.pull = WAZEH_CycleTime_Pull::Up;
    config.debounce_ms = 30;

    esp_err_t err = cycleTime.addChannel(config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "addChannel failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Momentary switch: GPIO%d to GND. Press/release to observe timings.", SWITCH_GPIO);
    ESP_LOGI(TAG, "This example uses the internal pull-up and 30 ms debounce.");

    bool previous_pressed = cycleTime.pressed(SWITCH_GPIO);
    while (true) {
        (void)cycleTime.loop();
        const bool is_pressed = cycleTime.pressed(SWITCH_GPIO);
        if (is_pressed != previous_pressed) {
            if (is_pressed) {
                ESP_LOGI(TAG, "PRESSED; event count=%lu", cycleTime.getEventCount(SWITCH_GPIO));
            } else {
                ESP_LOGI(TAG, "RELEASED; last event length=%lu ms; cycle time=%lu ms",
                         cycleTime.getEventLength(SWITCH_GPIO),
                         cycleTime.calculateCycleTime(SWITCH_GPIO));
            }
            previous_pressed = is_pressed;
        }
        const WAZEH_CycleTime_RunStopResult times = cycleTime.calculateRunStopTime(SWITCH_GPIO);
        (void)times; // Available here for application-specific reporting.
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
