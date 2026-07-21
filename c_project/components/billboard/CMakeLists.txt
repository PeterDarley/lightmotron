idf_component_register(
    SRCS "max7219.c" "billboard.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_gpio esp_driver_spi storage
)
