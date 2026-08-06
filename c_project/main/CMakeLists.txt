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
# "webassets" LittleFS partition so components/webserver can find them at
# /spiffs/www and /spiffs/templates (base_path is unchanged across the
# SPIFFS->LittleFS migration -- see LITTLEFS_MIGRATION_NOTES.md -- only the
# on-flash format changed). This is a SEPARATE partition from "data"
# (settings JSON, scenes, effects, colors, sounds, ...) specifically so that
# reflashing app/asset changes via `idf.py flash` / flash_c.ps1 never
# touches user-owned persistent storage -- FLASH_IN_PROJECT means this
# image is rebuilt and rewritten on every default flash.
#
# NOTE: this copy happens at CMake configure time. If you edit files under
# ../../www or ../../templates, run `idf.py reconfigure` (or touch this
# file) before `idf.py build` so the staged copy picks up the changes.
set(WEBASSETS_STAGING_DIR "${CMAKE_CURRENT_BINARY_DIR}/webassets_staging")
file(REMOVE_RECURSE "${WEBASSETS_STAGING_DIR}")
file(MAKE_DIRECTORY "${WEBASSETS_STAGING_DIR}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/../../www" DESTINATION "${WEBASSETS_STAGING_DIR}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/../../templates" DESTINATION "${WEBASSETS_STAGING_DIR}")

littlefs_create_partition_image(webassets "${WEBASSETS_STAGING_DIR}" FLASH_IN_PROJECT)
