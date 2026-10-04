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
# The copy below happens at CMake configure time. Registering every www/ and
# templates/ file as a configure dependency makes any edit trigger a
# reconfigure on the next build, so the staged copy can't go stale (it did
# once: a template edit was built and flashed without the change reaching
# the device). CONFIGURE_DEPENDS also picks up newly added files.
file(GLOB_RECURSE WEB_ASSET_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/../../www/*"
    "${CMAKE_CURRENT_LIST_DIR}/../../templates/*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${WEB_ASSET_FILES})

set(WEBASSETS_STAGING_DIR "${CMAKE_CURRENT_BINARY_DIR}/webassets_staging")
file(REMOVE_RECURSE "${WEBASSETS_STAGING_DIR}")
file(MAKE_DIRECTORY "${WEBASSETS_STAGING_DIR}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/../../www" DESTINATION "${WEBASSETS_STAGING_DIR}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/../../templates" DESTINATION "${WEBASSETS_STAGING_DIR}")

littlefs_create_partition_image(webassets "${WEBASSETS_STAGING_DIR}" FLASH_IN_PROJECT)
