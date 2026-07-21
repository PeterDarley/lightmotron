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
# "storage" SPIFFS partition so components/webserver can find them at
# /spiffs/www and /spiffs/templates. Settings JSON files under /spiffs/data
# don't need to be pre-staged here — persistent_dict_open() creates them on
# first write, same as lib/storage.py.
#
# NOTE: this copy happens at CMake configure time. If you edit files under
# ../../www or ../../templates, run `idf.py reconfigure` (or touch this
# file) before `idf.py build` so the staged copy picks up the changes.
set(SPIFFS_STAGING_DIR "${CMAKE_CURRENT_BINARY_DIR}/spiffs_staging")
file(REMOVE_RECURSE "${SPIFFS_STAGING_DIR}")
file(MAKE_DIRECTORY "${SPIFFS_STAGING_DIR}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/../../www" DESTINATION "${SPIFFS_STAGING_DIR}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/../../templates" DESTINATION "${SPIFFS_STAGING_DIR}")

spiffs_create_partition_image(storage "${SPIFFS_STAGING_DIR}" FLASH_IN_PROJECT)
