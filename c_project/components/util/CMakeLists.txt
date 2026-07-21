idf_component_register(
    SRCS "timing.c" "comms.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_gpio esp_wifi esp_netif esp_timer
)
