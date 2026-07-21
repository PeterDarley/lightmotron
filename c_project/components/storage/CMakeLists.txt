idf_component_register(
    SRCS "persistent_dict.c" "json_helpers.c"
    INCLUDE_DIRS "include"
    REQUIRES spiffs espressif__cjson
)
