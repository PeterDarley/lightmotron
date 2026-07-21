idf_component_register(
    SRCS "audio_player.c" "yx5200.c" "sound_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_uart storage
)
