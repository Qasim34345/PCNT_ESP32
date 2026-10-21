idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        WAZEH_CycleTime
        esp_driver_gpio
        esp_timer
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
