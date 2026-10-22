/*
 =========================================================================================
 * File:        WAZEH_CycleTime_Config.h
 * Author:      AMS-IoT
 * Version:     Rev_1.1.0
 * Date:        Oct 21 2026
 * Brief:       This file provides WAZEH CycleTime configuration options.
 =========================================================================================
 * License:
 *
 * PROPRIETARY AND CONFIDENTIAL
 *
 * Copyright (c) 2025 AMS-IoT.
 * All rights reserved.
 *
 * This software and associated documentation are the exclusive property of AMS-IoT.
 * Unauthorized copying, modification, distribution, sublicensing, disclosure, or use
 * of this software, in whole or in part, is strictly prohibited.
 *
 * No license, express or implied, is granted under any patent, copyright, trademark,
 * or other intellectual property right of AMS-IoT, except as expressly authorized
 * in writing by AMS-IoT.
 *
 * This software is provided solely for use with authorized AMS-IoT products or
 * projects and may not be used for any other purpose without prior written consent.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND WITHOUT WARRANTY OF ANY KIND. AMS-IoT SHALL NOT
 * BE LIABLE FOR ANY DAMAGES ARISING FROM THE USE OR INABILITY TO USE THIS SOFTWARE.
 *
 * Violations of this license may result in civil and/or criminal liability under
 * applicable laws.
 *
 * For authorization or licensing inquiries, contact: AMS-IoT
 =========================================================================================
 */


#ifndef WAZEH_CYCLETIME_CONFIG_H
#define WAZEH_CYCLETIME_CONFIG_H

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <functional>
#include <stddef.h>
#include <esp_timer.h>
#include <esp_err.h>
#include <driver/gpio.h>
#include <driver/pulse_cnt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/*
  ┌─────────────────────────────────────────────────────────────────────┐
  │ Note:     Enable only if ChronoLog is included                      │
  │ Requires: ChronoLog library → https://github.com/Hamas888/ChronoLog │
  └─────────────────────────────────────────────────────────────────────┘
*/
#if defined(CONFIG_WAZEH_CYCLETIME_DEBUG)
    #define WAZEH_CYCLETIME_DEBUG_ENABLED           1
#elif defined(WAZEH_CYCLETIME_DEBUG)
    #define WAZEH_CYCLETIME_DEBUG_ENABLED           1
#else
    #define WAZEH_CYCLETIME_DEBUG_ENABLED           0                            // Enable debug messages (1=enabled, 0=disabled)
#endif


#if WAZEH_CYCLETIME_DEBUG_ENABLED
  #if __has_include("ChronoLog.h")
    #include "ChronoLog.h"
    #ifndef WAZEH_CYCLETIME_LOG_LEVEL
      #define WAZEH_CYCLETIME_LOG_LEVEL             CHRONOLOG_LEVEL_DEBUG
    #endif
    extern ChronoLogger *wazehCycleTimeLogger;
  #else
    #error "WAZEH_CYCLETIME_DEBUG is enabled but    \
    ChronoLog.h is missing. Please include the      \
    https://github.com/Hamas888/ChronoLog           \
    in your project."
  #endif
#endif

#if WAZEH_CYCLETIME_DEBUG_ENABLED
  #define WAZEH_CYCLETIME_LOGGER(level, msg, ...)   \
    do {                                            \
      if (wazehCycleTimeLogger)                     \
        wazehCycleTimeLogger->level(                \
          msg, ##__VA_ARGS__                        \
        );                                          \
    } while (0)
  #else
    #define WAZEH_CYCLETIME_LOGGER(level, msg, ...) do {} while (0)
#endif


/*
  ┌─────────────────────────────────────────────────────────────────────┐
  │ Note: WAZEH CycleTime Custom Types & Definitions                    │
  └─────────────────────────────────────────────────────────────────────┘
*/

#define WAZEH_CYCLETIME_REPEAT_DELAY                500    // Repeat event delay in ms

#ifndef WAZEH_CYCLETIME_MAX_CHANNELS
#define WAZEH_CYCLETIME_MAX_CHANNELS                16     // Fixed-capacity channel table; avoids vector allocation in the component
#endif
#if WAZEH_CYCLETIME_MAX_CHANNELS < 1
#error "WAZEH_CYCLETIME_MAX_CHANNELS must be at least 1"
#endif
#define WAZEH_CYCLETIME_DEBOUNCE_DELAY              50     // Debounce delay in ms

#define WAZEH_CYCLETIME_CHANNEL_PULLUP              4      // Enable internal pull-up resistor
#define WAZEH_CYCLETIME_CHANNEL_PULLDOWN            0      // Enable internal pull-down resistor

#define WAZEH_CYCLETIME_CHANNEL_DEFAULT_LOW         0      // Default state is LOW
#define WAZEH_CYCLETIME_CHANNEL_DEFAULT_HIGH        2      // Default state is HIGH

typedef enum {
    CYCLETIME_EVENT_NONE       = 0,
    CYCLETIME_EVENT_CHANGED    = 1,
    CYCLETIME_EVENT_PRESSED    = 2,
    CYCLETIME_EVENT_RELEASED   = 3,
    CYCLETIME_EVENT_REPEAT     = 4
} WAZEH_CycleTime_Event;

typedef enum {
    CYCLETIME_INPUT_MODE_SWITCH       = 1,
    CYCLETIME_INPUT_MODE_PUSHBUTTON   = 0
} WAZEH_CycleTime_InputMode;

enum class WAZEH_CycleTime_ActiveLevel : uint8_t {
    Low = 0,
    High = 1
};

enum class WAZEH_CycleTime_Pull : uint8_t {
    None = 0,
    Up,
    Down
};

enum class WAZEH_CycleTime_CounterBackend : uint8_t {
    Unavailable = 0,
    HardwarePcnt,
    Interrupt,
    Polling = Interrupt
};

enum class WAZEH_CycleTime_Edge : uint8_t {
    Rising = 0,
    Falling,
    Both
};

struct WAZEH_CycleTime_ChannelConfig {
    gpio_num_t pin = GPIO_NUM_NC;
    WAZEH_CycleTime_InputMode input_mode = CYCLETIME_INPUT_MODE_PUSHBUTTON;
    WAZEH_CycleTime_ActiveLevel active_level = WAZEH_CycleTime_ActiveLevel::High;
    WAZEH_CycleTime_Pull pull = WAZEH_CycleTime_Pull::None;
    uint32_t debounce_ms = WAZEH_CYCLETIME_DEBOUNCE_DELAY;
};

typedef struct {
    unsigned long run_time;   /**< Total time machine was running (ms) */
    unsigned long stop_time;  /**< Total time machine was stopped (ms) */
} WAZEH_CycleTime_RunStopResult ;

struct WAZEH_CycleTime_EventData {
    gpio_num_t pin;
    WAZEH_CycleTime_Event event;
    uint64_t event_count;
    uint64_t event_length_ms;
    int64_t timestamp_us;
};

using WAZEH_CycleTime_TypedEventCallback =
    std::function<void(const WAZEH_CycleTime_EventData &)>;

typedef std::function<void(uint8_t pin, uint8_t event, uint8_t count, uint16_t length)> WAZEH_CycleTime_Event_Callback; 

#endif // WAZEH_CYCLETIME_CONFIG_H