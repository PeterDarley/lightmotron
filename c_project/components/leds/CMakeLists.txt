idf_component_register(
    SRCS "leds.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_rmt esp_driver_gpio storage
)
