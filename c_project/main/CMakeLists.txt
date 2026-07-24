idf_component_register(
    SRCS "main.c" "boot.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
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

# Package the shared www/ and templates/ directories (the same assets the
# Python build serves, kept one level up at the project root) into the
# "webassets" SPIFFS partition so components/webserver can find them at
# /spiffs/www and /spiffs/templates. This is a SEPARATE partition from
# "data" (settings JSON, scenes, effects, colors, sounds, ...) specifically
# so that reflashing app/asset changes via `idf.py flash` / flash_c.ps1
# never touches user-owned persistent storage -- FLASH_IN_PROJECT means
# this image is rebuilt and rewritten on every default flash.
#
# NOTE: this copy happens at CMake configure time. If you edit files under
# ../../www or ../../templates, run `idf.py reconfigure` (or touch this
# file) before `idf.py build` so the staged copy picks up the changes.
set(SPIFFS_STAGING_DIR "${CMAKE_CURRENT_BINARY_DIR}/spiffs_staging")
file(REMOVE_RECURSE "${SPIFFS_STAGING_DIR}")
file(MAKE_DIRECTORY "${SPIFFS_STAGING_DIR}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/../../www" DESTINATION "${SPIFFS_STAGING_DIR}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/../../templates" DESTINATION "${SPIFFS_STAGING_DIR}")

spiffs_create_partition_image(webassets "${SPIFFS_STAGING_DIR}" FLASH_IN_PROJECT)
