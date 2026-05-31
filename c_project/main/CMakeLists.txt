idf_component_register(
    SRCS "main.c" "boot.c"
    INCLUDE_DIRS "."
    REQUIRES
        storage
        webserver
        lighting
        leds
        audio
        billboard
        network
        web
        util
)
