idf_component_register(
    SRCS "test_main.cpp"
    PRIV_REQUIRES unity WAZEH_CycleTime esp_driver_gpio esp_timer freertos
    WHOLE_ARCHIVE
)
