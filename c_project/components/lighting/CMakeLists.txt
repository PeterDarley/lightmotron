idf_component_register(
    SRCS "animation.c" "lighting.c" "patterns.c" "filters.c" "effects.c" "colors.c" "named_ranges.c" "metadata.c"
    INCLUDE_DIRS "include"
    REQUIRES leds storage audio
)
