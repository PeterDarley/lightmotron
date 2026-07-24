idf_component_register(
    SRCS "wifi_manager.c" "captive_portal.c" "mdns_setup.c" "ota_update.c" "ip_announcement.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_http_client esp_https_ota mdns lwip esp_event app_update storage audio leds mbedtls
)

# Embed the current commit/tag as FIRMWARE_VERSION for ota_update.c's
# GitHub-release version comparison. `--always` falls back to the
# abbreviated commit hash when no tags exist yet (which is the case today);
# `--tags` picks up lightweight tags too, not just annotated ones; `--dirty`
# flags a build made from uncommitted changes so it's never mistaken for a
# clean tagged release.
execute_process(
    COMMAND git describe --always --tags --dirty
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
    OUTPUT_VARIABLE FIRMWARE_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT FIRMWARE_VERSION)
    set(FIRMWARE_VERSION "unknown")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE FIRMWARE_VERSION="${FIRMWARE_VERSION}")
