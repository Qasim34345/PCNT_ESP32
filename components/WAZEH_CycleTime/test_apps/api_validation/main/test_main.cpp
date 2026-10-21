#include "unity.h"
#include "unity_test_utils.h"
#include "WAZEH_CycleTime.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"

namespace {
constexpr gpio_num_t TEST_OUTPUT_GPIO = GPIO_NUM_4;
constexpr gpio_num_t TEST_INPUT_GPIO = GPIO_NUM_5;

void configureTestOutput()
{
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << TEST_OUTPUT_GPIO;
    config.mode = GPIO_MODE_OUTPUT;
    config.intr_type = GPIO_INTR_DISABLE;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    TEST_ASSERT_EQUAL(ESP_OK, gpio_config(&config));
    TEST_ASSERT_EQUAL(ESP_OK, gpio_set_level(TEST_OUTPUT_GPIO, 0));
}

void emitRisingEdgePulses(unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        TEST_ASSERT_EQUAL(ESP_OK, gpio_set_level(TEST_OUTPUT_GPIO, 1));
        vTaskDelay(pdMS_TO_TICKS(20));
        TEST_ASSERT_EQUAL(ESP_OK, gpio_set_level(TEST_OUTPUT_GPIO, 0));
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(20));
}

WAZEH_CycleTime_ChannelConfig makeInputConfig()
{
    WAZEH_CycleTime_ChannelConfig config{};
    config.pin = TEST_INPUT_GPIO;
    config.input_mode = CYCLETIME_INPUT_MODE_SWITCH;
    config.active_level = WAZEH_CycleTime_ActiveLevel::High;
    config.pull = WAZEH_CycleTime_Pull::Down;
    config.debounce_ms = 0;
    return config;
}
}

TEST_CASE("typed channel API rejects invalid pins", "[wazeh][api]")
{
    WAZEH_CycleTime cycleTime;
    WAZEH_CycleTime_ChannelConfig config{};
    config.pin = GPIO_NUM_NC;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cycleTime.addChannel(config));
    config.pin = GPIO_NUM_5;
    config.active_level = static_cast<WAZEH_CycleTime_ActiveLevel>(99);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cycleTime.addChannel(config));
    uint64_t count = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, cycleTime.getPulseCount(GPIO_NUM_5, &count));
    TEST_ASSERT_EQUAL(ESP_OK, cycleTime.deinit());
}

TEST_CASE("default constructor safely supports typed channel addition", "[wazeh][api]")
{
    WAZEH_CycleTime cycleTime;
    TEST_ASSERT_EQUAL(ESP_OK, cycleTime.addChannel(makeInputConfig()));
    TEST_ASSERT_NOT_NULL(cycleTime.getChannel(GPIO_NUM_5));
    (void)cycleTime.loop();
    TEST_ASSERT_EQUAL(ESP_OK, cycleTime.deinit());
}

TEST_CASE("PCNT initialization and channel cleanup can be repeated", "[wazeh][pcnt]")
{
    for (int attempt = 0; attempt < 3; ++attempt) {
        WAZEH_CycleTime cycleTime;
        TEST_ASSERT_EQUAL(ESP_OK, cycleTime.addChannel(makeInputConfig()));

#if SOC_PCNT_SUPPORTED
        TEST_ASSERT_EQUAL(
            ESP_OK,
            cycleTime.initPulseCounterByPinChecked(
                TEST_INPUT_GPIO, WAZEH_CycleTime_Edge::Rising, 0, false));
        uint64_t count = 0;
        TEST_ASSERT_EQUAL(ESP_OK, cycleTime.getPulseCount(TEST_INPUT_GPIO, &count));
        TEST_ASSERT_EQUAL_UINT64(0, count);
#else
        TEST_ASSERT_EQUAL(
            ESP_ERR_NOT_SUPPORTED,
            cycleTime.initPulseCounterByPinChecked(
                TEST_INPUT_GPIO, WAZEH_CycleTime_Edge::Rising, 0, false));
#endif
        TEST_ASSERT_EQUAL(ESP_OK, cycleTime.deinit());
    }
}

TEST_CASE("hardware PCNT counts rising edges on GPIO loopback", "[wazeh][pcnt][pulse]")
{
#if SOC_PCNT_SUPPORTED
    configureTestOutput();
    WAZEH_CycleTime cycleTime;
    TEST_ASSERT_EQUAL(ESP_OK, cycleTime.addChannel(makeInputConfig()));
    TEST_ASSERT_EQUAL(ESP_OK, cycleTime.initPulseCounterByPinChecked(
        TEST_INPUT_GPIO, WAZEH_CycleTime_Edge::Rising, 0, false));
    TEST_ASSERT_TRUE(cycleTime.getChannel(TEST_INPUT_GPIO)->isPcntInitialized());

    constexpr uint64_t expected = 8;
    emitRisingEdgePulses(static_cast<unsigned>(expected));
    uint64_t count = 0;
    TEST_ASSERT_EQUAL(ESP_OK, cycleTime.getPulseCount(TEST_INPUT_GPIO, &count));
    TEST_ASSERT_EQUAL_UINT64(expected, count);
    TEST_ASSERT_EQUAL(ESP_OK, cycleTime.deinit());
#else
    TEST_IGNORE_MESSAGE("Target does not provide PCNT hardware");
#endif
}

TEST_CASE("GPIO interrupt fallback counts rising edges on GPIO loopback", "[wazeh][interrupt][pulse]")
{
    configureTestOutput();
    WAZEH_CycleTime cycleTime;
    TEST_ASSERT_EQUAL(ESP_OK, cycleTime.addChannel(makeInputConfig()));
    WAZEH_Channel *channel = cycleTime.getChannel(TEST_INPUT_GPIO);
    TEST_ASSERT_NOT_NULL(channel);
    TEST_ASSERT_EQUAL(ESP_OK, channel->useSoftwareCounterChecked());
    TEST_ASSERT_EQUAL(WAZEH_CycleTime_CounterBackend::Interrupt, channel->getCounterBackend());

    constexpr uint64_t expected = 8;
    emitRisingEdgePulses(static_cast<unsigned>(expected));
    uint64_t count = 0;
    TEST_ASSERT_EQUAL(ESP_OK, cycleTime.getPulseCount(TEST_INPUT_GPIO, &count));
    TEST_ASSERT_EQUAL_UINT64(expected, count);
    TEST_ASSERT_EQUAL(ESP_OK, cycleTime.deinit());
}

TEST_CASE("pulse reset preserves cumulative total", "[wazeh][pulse]")
{
    configureTestOutput();
    WAZEH_CycleTime cycleTime;
    TEST_ASSERT_EQUAL(ESP_OK, cycleTime.addChannel(makeInputConfig()));
    WAZEH_Channel *channel = cycleTime.getChannel(TEST_INPUT_GPIO);
    TEST_ASSERT_NOT_NULL(channel);
    TEST_ASSERT_EQUAL(ESP_OK, channel->useSoftwareCounterChecked());

    emitRisingEdgePulses(3);
    uint64_t before = 0;
    TEST_ASSERT_EQUAL(ESP_OK, cycleTime.getPulseCount(TEST_INPUT_GPIO, &before));
    TEST_ASSERT_EQUAL_UINT64(3, before);
    TEST_ASSERT_EQUAL(ESP_OK, cycleTime.resetPulseCountChecked(TEST_INPUT_GPIO));
    emitRisingEdgePulses(2);
    uint64_t after = 0;
    TEST_ASSERT_EQUAL(ESP_OK, cycleTime.getPulseCount(TEST_INPUT_GPIO, &after));
    TEST_ASSERT_EQUAL_UINT64(5, after);
    TEST_ASSERT_EQUAL(ESP_OK, cycleTime.deinit());
}

extern "C" void app_main(void)
{
    unity_run_menu();
}
