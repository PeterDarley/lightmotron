idf_component_register(
    SRCS "timing.c" "comms.c" "utils.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_wifi esp_netif
)
