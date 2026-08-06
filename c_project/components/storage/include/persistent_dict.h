#ifndef PERSISTENT_DICT_H
#define PERSISTENT_DICT_H

#include "cJSON.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdbool.h>

/**
 * Canonical storage file paths. Every component that opens a persistent_dict
 * should use these rather than redefining the path locally, so there is a
 * single source of truth for where each settings file lives.
 *
 * These live on the "data" LittleFS partition (mounted at /data in
 * main/boot.c), which is deliberately separate from the "webassets"
 * partition (www/templates, mounted at /spiffs) so that reflashing
 * app/asset changes via `idf.py flash` never touches this data.
 */
#define STORAGE_SYSTEM_SETTINGS_FILE "/data/system_settings.json"
#define STORAGE_LIGHTING_SETTINGS_FILE "/data/lighting_settings.json"
#define STORAGE_SOUNDS_FILE "/data/sounds.json"

/**
 * Lazy-loaded persistent dictionary backed by a JSON file on LittleFS.
 *
 * Data is only loaded from disk on first access, not during initialization.
 */
typedef struct {
    char filepath[128];
    cJSON *data;
    bool loaded;
    bool dirty;
    SemaphoreHandle_t mutex;
} persistent_dict_t;

/**
 * Open (or get existing) persistent dict for the given file path.
 *
 * Returns a singleton per filepath. Thread-safe.
 */
persistent_dict_t *persistent_dict_open(const char *filepath);

/**
 * Get a value by key. Returns NULL if key not found.
 *
 * The returned cJSON pointer is owned by the dict — do not free it.
 * WARNING: Not safe if another thread may modify the same key concurrently.
 * Use persistent_dict_get_dup() in multi-threaded contexts.
 *
 * WARNING: mutating the returned tree directly (cJSON_AddItemToObject,
 * cJSON_DeleteItemFromObject, etc. on it or a sub-object within it) DOES
 * update the dict's in-memory state, since this is a live pointer, not a
 * copy -- but it does NOT mark the dict dirty, so persistent_dict_save()
 * will silently no-op and the change is lost on reboot. Call
 * persistent_dict_mark_dirty() after any such direct mutation, before
 * saving. (Confirmed as a real bug 2026-08-05: a scene's "active on boot"
 * toggle and its kills/trigger-scenes editor both mutated a live tree this
 * way with no matching mark_dirty call -- the change looked saved
 * immediately in the UI, since the UI reads the same in-memory tree, but
 * reverted on every reboot.)
 */
cJSON *persistent_dict_get(persistent_dict_t *pd, const char *key);

/**
 * Get a deep copy of a value by key. Returns NULL if key not found.
 *
 * The returned cJSON is owned by the caller and must be freed with cJSON_Delete().
 * Thread-safe against concurrent modifications.
 */
cJSON *persistent_dict_get_dup(persistent_dict_t *pd, const char *key);

/**
 * Set a value by key. The cJSON value is adopted (owned by the dict after this call).
 */
void persistent_dict_set(persistent_dict_t *pd, const char *key, cJSON *value);

/**
 * Delete a key from the dict.
 */
void persistent_dict_delete_key(persistent_dict_t *pd, const char *key);

/**
 * Mark the dict dirty without changing anything -- needed after directly
 * mutating a tree obtained via persistent_dict_get() (a live pointer, not
 * a copy), since that mutation alone doesn't set the dirty flag the way
 * persistent_dict_set()/persistent_dict_delete_key() do. See
 * persistent_dict_get()'s doc comment.
 */
void persistent_dict_mark_dirty(persistent_dict_t *pd);

/**
 * Check if a key exists.
 */
bool persistent_dict_has_key(persistent_dict_t *pd, const char *key);

/**
 * Save the current state to disk (LittleFS).
 */
esp_err_t persistent_dict_save(persistent_dict_t *pd);

/**
 * Force reload from disk on next access.
 */
void persistent_dict_invalidate(persistent_dict_t *pd);

/**
 * Get the entire data object (for iteration). Do not free.
 */
cJSON *persistent_dict_get_all(persistent_dict_t *pd);

#endif /* PERSISTENT_DICT_H */
