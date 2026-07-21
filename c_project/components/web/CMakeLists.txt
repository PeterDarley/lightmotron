idf_component_register(
    SRCS "routes.c" "views_home.c" "views_scenes.c" "views_effects.c"
         "views_filters.c" "views_sounds.c" "views_soundscapes.c"
         "views_named_ranges.c" "views_system.c" "views_models.c"
         "views_storage.c" "context_processors.c"
    INCLUDE_DIRS "include"
    REQUIRES webserver storage lighting audio billboard util network
)
