# WAZEH_CycleTime/CMakeLists.txt

set(WAZEH_CYCLETIME_VERSION 1.0.0)

if(DEFINED ESP_PLATFORM OR DEFINED IDF_VER OR DEFINED ENV{IDF_PATH})
    idf_component_register(
        INCLUDE_DIRS "include"
        SRCS 
            "src/WAZEH_Channel.cpp"
            "src/WAZEH_CycleTime.cpp" 
        REQUIRES 
            "esp_driver_gpio"
            "esp_driver_pcnt"
            "freertos"
            "esp_timer"
    )

    # Force C++17 standard
    target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

    # (optional) if you want strict enforcement instead of compiler default fallback
    set_target_properties(${COMPONENT_LIB} PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
endif()