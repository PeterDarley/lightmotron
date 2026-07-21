idf_component_register(
    SRCS "webserver.c" "template_engine.c" "request_parser.c" "static_files.c" "mime_types.c"
    INCLUDE_DIRS "include"
    REQUIRES lwip esp_netif storage
)
